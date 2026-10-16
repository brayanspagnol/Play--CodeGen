#include "WasmModuleBuilder.h"
#include <cassert>
#include <cstring>
#include "WasmDefs.h"
#include "MemStream.h"
#include <cstdio>
#include <algorithm>

static void WriteName(Framework::CStream& stream, const char* str)
{
	auto length = strlen(str);
	assert(length < 0x80);
	stream.Write8(length);
	stream.Write(str, length);
}

void CWasmModuleBuilder::WriteSLeb128(Framework::CStream& stream, int64 value)
{
	bool more = true;
	while(more)
	{
		uint8 byte = (value & 0x7F);
		value >>= 7;
		if(
		    (value == 0 && ((byte & 0x40) == 0)) ||
		    (value == -1 && ((byte & 0x40) != 0)))
		{
			more = false;
		}
		else
		{
			byte |= 0x80;
		}
		stream.Write8(byte);
	}
}

void CWasmModuleBuilder::WriteULeb128(Framework::CStream& stream, uint64 value)
{
	while(1)
	{
		uint8 byte = (value & 0x7F);
		value >>= 7;
		if(value != 0)
		{
			byte |= 0x80;
		}
		stream.Write8(byte);
		if(value == 0)
		{
			break;
		}
	}
}

uint32 CWasmModuleBuilder::GetULeb128Size(uint32 value)
{
	uint32 size = 0;
	while(1)
	{
		value >>= 7;
		size++;
		if(value == 0)
		{
			break;
		}
	}
	return size;
}

void CWasmModuleBuilder::AddFunctionType(FUNCTION_TYPE functionType)
{
	m_functionTypes.push_back(std::move(functionType));
}

void CWasmModuleBuilder::AddFunction(FUNCTION function)
{
	m_functions.push_back(std::move(function));
}

void CWasmModuleBuilder::SetImportsBlockTable(bool importsBlockTable)
{
	m_importsBlockTable = importsBlockTable;
}

void CWasmModuleBuilder::SetFunctionTypeIndex(uint32 index)
{
	m_functionTypeIndex = index;
}

void CWasmModuleBuilder::WriteModule(Framework::CStream& stream)
{
	//We only support a single function at the moment
	assert(m_functions.size() == 1);

	stream.Write32(Wasm::BINARY_MAGIC);
	stream.Write32(Wasm::BINARY_VERSION);

	//Section "Type"
	{
		uint32 sectionSize = 0;
		sectionSize += GetULeb128Size(m_functionTypes.size());
		for(const auto& functionType : m_functionTypes)
		{
			sectionSize +=
			    1 + //Type
			    GetULeb128Size(functionType.params.size()) +
			    static_cast<uint32>(functionType.params.size()) +
			    GetULeb128Size(functionType.results.size()) +
			    static_cast<uint32>(functionType.results.size());
		}

		WriteSectionHeader(stream, Wasm::SECTION_ID_TYPE, sectionSize);

		//Vector size
		WriteULeb128(stream, m_functionTypes.size());

		for(const auto& functionType : m_functionTypes)
		{
			stream.Write8(0x60); //Func

			WriteULeb128(stream, functionType.params.size()); //Num params
			for(uint32 i = 0; i < functionType.params.size(); i++)
			{
				stream.Write8(functionType.params[i]);
			}

			WriteULeb128(stream, functionType.results.size()); //Num results
			for(uint32 i = 0; i < functionType.results.size(); i++)
			{
				stream.Write8(functionType.results[i]);
			}
		}
	}

	WriteImportSection(stream);

	//Section "Function"
	{
		WriteSectionHeader(stream, Wasm::SECTION_ID_FUNCTION, 0x02);

		stream.Write8(1); //Function vector size

		//Function 0
		assert(m_functionTypeIndex < 0x80);
		stream.Write8(m_functionTypeIndex); //Signature index
	}

	//Section "Export"
	{
		WriteSectionHeader(stream, Wasm::SECTION_ID_EXPORT, 15);

		stream.Write8(1); //Export vector size

		//Export 0
		WriteName(stream, "codeGenFunc");
		stream.Write8(Wasm::IMPORT_EXPORT_TYPE_FUNCTION);
		stream.Write8(0); //Function index
	}

	//Section "Code"
	{
		const auto& function = m_functions[0];

		assert(function.localI32Count < 0x80);
		assert(function.localI64Count < 0x80);
		assert(function.localF32Count < 0x80);
		assert(function.localV128Count < 0x80);

		uint32 localDeclCount = 0;
		if(function.localI32Count != 0) localDeclCount++;
		if(function.localI64Count != 0) localDeclCount++;
		if(function.localF32Count != 0) localDeclCount++;
		if(function.localV128Count != 0) localDeclCount++;
		uint32 localDeclSize = 1;
		for(uint32 count : {function.localI32Count, function.localI64Count, function.localF32Count, function.localV128Count})
		{
			if(count != 0) localDeclSize += GetULeb128Size(count) + 1;
		}
		uint32 functionBodySize = function.code.size() + localDeclSize;

		uint32 sectionSize =
		    1 + //Vector size
		    GetULeb128Size(functionBodySize) +
		    functionBodySize;

		WriteSectionHeader(stream, Wasm::SECTION_ID_CODE, sectionSize);

		stream.Write8(1); //Function vector size

		//Function 0
		WriteULeb128(stream, functionBodySize); //Function body size
		WriteULeb128(stream, localDeclCount);   //Local declaration count
		if(function.localI32Count != 0)
		{
			WriteULeb128(stream, function.localI32Count); //Local type count
			stream.Write8(Wasm::TYPE_I32);
		}
		if(function.localI64Count != 0)
		{
			WriteULeb128(stream, function.localI64Count);
			stream.Write8(Wasm::TYPE_I64);
		}
		if(function.localF32Count != 0)
		{
			WriteULeb128(stream, function.localF32Count);
			stream.Write8(Wasm::TYPE_F32);
		}
		if(function.localV128Count != 0)
		{
			WriteULeb128(stream, function.localV128Count);
			stream.Write8(Wasm::TYPE_V128);
		}

		stream.Write(function.code.data(), function.code.size());
	}

	//Custom section "name" (function names subsection), lets profilers tell blocks apart
	if(!m_functions[0].name.empty())
	{
		const auto& name = m_functions[0].name;
		uint32 nameSize = static_cast<uint32>(name.size());
		uint32 subsectionSize = 1 + 1 + GetULeb128Size(nameSize) + nameSize; //count, index, name
		uint32 sectionSize = 1 + 4 + 1 + GetULeb128Size(subsectionSize) + subsectionSize;
		WriteSectionHeader(stream, 0, sectionSize);
		WriteName(stream, "name");
		stream.Write8(1); //Function names
		WriteULeb128(stream, subsectionSize);
		stream.Write8(1); //Count
		stream.Write8(0); //Function index
		WriteULeb128(stream, nameSize);
		stream.Write(name.data(), nameSize);
	}
}

void CWasmModuleBuilder::WriteSectionHeader(Framework::CStream& stream, uint8 sectionId, uint32 size)
{
	stream.Write8(sectionId);
	WriteULeb128(stream, size);
}

void CWasmModuleBuilder::WriteImportSection(Framework::CStream& stream)
{
	//Table import: module name (1 + 3), field name (1 + n), kind, reftype, flags, initial
	const uint32 blockTableImportSize = 4 + 1 + 10 + 4;
	WriteSectionHeader(stream, Wasm::SECTION_ID_IMPORT, 0x23 + (m_importsBlockTable ? blockTableImportSize : 0));

	stream.Write8(m_importsBlockTable ? 3 : 2); //Import vector size

	//Import 0
	WriteName(stream, "env");    //Import module name
	WriteName(stream, "memory"); //Import field name
	stream.Write8(Wasm::IMPORT_EXPORT_TYPE_MEMORY);
	stream.Write8(0x03);         //Limit type 3 (0x01 = has max, 0x02 = is shared?)
	stream.Write8(0x01);         //Min
	WriteULeb128(stream, 32768); //Max (default Emscripten value)

	//Import 1
	WriteName(stream, "env");
	WriteName(stream, "fctTable");
	stream.Write8(Wasm::IMPORT_EXPORT_TYPE_TABLE);
	stream.Write8(0x70); //Funcref
	stream.Write8(0x00); //Flags
	stream.Write8(0x01); //Initial

	if(m_importsBlockTable)
	{
		WriteName(stream, "env");
		WriteName(stream, "blockTable");
		stream.Write8(Wasm::IMPORT_EXPORT_TYPE_TABLE);
		stream.Write8(0x70); //Funcref
		stream.Write8(0x00); //Flags
		stream.Write8(0x01); //Initial
	}
}

static bool ReadULeb128(const uint8*& ptr, const uint8* end, uint32& value)
{
	value = 0;
	for(uint32 shift = 0; shift < 35; shift += 7)
	{
		if(ptr >= end) return false;
		uint8 byte = *ptr++;
		value |= static_cast<uint32>(byte & 0x7F) << shift;
		if((byte & 0x80) == 0) return true;
	}
	return false;
}

std::vector<uint8> CWasmModuleBuilder::BuildBatchModule(const std::vector<MODULE_BYTES>& modules)
{
	std::vector<BATCH_FUNCTION> functions(modules.size());
	std::vector<uint32> exports(modules.size());
	for(uint32 i = 0; i < modules.size(); i++)
	{
		functions[i].modules = {i};
		exports[i] = i;
	}
	return BuildBatchModule(modules, functions, exports, REGION_PARAMS());
}

static bool ReadSLeb(const uint8*& ptr, const uint8* end, uint32 maxBytes)
{
	for(uint32 i = 0; i < maxBytes; i++)
	{
		if(ptr >= end) return false;
		if((*ptr++ & 0x80) == 0) return true;
	}
	return false;
}

static bool SkipBytes(const uint8*& ptr, const uint8* end, uint32 count)
{
	if(static_cast<uint32>(end - ptr) < count) return false;
	ptr += count;
	return true;
}

static bool SkipMemArg(const uint8*& ptr, const uint8* end)
{
	uint32 align = 0, offset = 0;
	if(!ReadULeb128(ptr, end, align)) return false;
	if(align & 0x40)
	{
		uint32 memory = 0;
		if(!ReadULeb128(ptr, end, memory)) return false;
	}
	return ReadULeb128(ptr, end, offset);
}

//Skips the immediates of the instruction whose opcode was just read. Covers what the Jitter emits
//(MVP, sign extension, saturating conversions, bulk memory, SIMD, tail calls); anything else fails.
static bool SkipImmediates(uint8 op, const uint8*& ptr, const uint8* end)
{
	uint32 value = 0;
	switch(op)
	{
	case 0x02: //block
	case 0x03: //loop
	case 0x04: //if
		return ReadSLeb(ptr, end, 5);
	case 0x0C: //br
	case 0x0D: //br_if
	case 0x10: //call
	case 0x12: //return_call
	case 0x23: //global.get
	case 0x24: //global.set
	case 0x25: //table.get
	case 0x26: //table.set
	case 0xD2: //ref.func
		return ReadULeb128(ptr, end, value);
	case 0x0E: //br_table
	{
		uint32 count = 0;
		if(!ReadULeb128(ptr, end, count)) return false;
		for(uint32 i = 0; i <= count; i++)
		{
			if(!ReadULeb128(ptr, end, value)) return false;
		}
		return true;
	}
	case 0x11: //call_indirect
	case 0x13: //return_call_indirect
		return ReadULeb128(ptr, end, value) && ReadULeb128(ptr, end, value);
	case 0x1C: //select t*
	{
		uint32 count = 0;
		if(!ReadULeb128(ptr, end, count)) return false;
		return SkipBytes(ptr, end, count);
	}
	case 0x3F: //memory.size
	case 0x40: //memory.grow
		return ReadULeb128(ptr, end, value);
	case 0x41: //i32.const
		return ReadSLeb(ptr, end, 5);
	case 0x42: //i64.const
		return ReadSLeb(ptr, end, 10);
	case 0x43: //f32.const
		return SkipBytes(ptr, end, 4);
	case 0x44: //f64.const
		return SkipBytes(ptr, end, 8);
	case 0xD0: //ref.null
		return SkipBytes(ptr, end, 1);
	case 0xFC:
	{
		uint32 subOp = 0;
		if(!ReadULeb128(ptr, end, subOp)) return false;
		if(subOp <= 7) return true;                                                                        //trunc_sat
		if(subOp == 8) return ReadULeb128(ptr, end, value) && SkipBytes(ptr, end, 1);                      //memory.init
		if(subOp == 9 || subOp == 13 || (subOp >= 15 && subOp <= 17)) return ReadULeb128(ptr, end, value); //data.drop, elem.drop, table.grow/size/fill
		if(subOp == 10) return SkipBytes(ptr, end, 2);                                                     //memory.copy
		if(subOp == 11) return SkipBytes(ptr, end, 1);                                                     //memory.fill
		if(subOp == 12 || subOp == 14) return ReadULeb128(ptr, end, value) && ReadULeb128(ptr, end, value); //table.init, table.copy
		return false;
	}
	case 0xFD:
	{
		uint32 subOp = 0;
		if(!ReadULeb128(ptr, end, subOp)) return false;
		if(subOp <= 11) return SkipMemArg(ptr, end);                              //loads, store
		if(subOp == 12 || subOp == 13) return SkipBytes(ptr, end, 16);            //v128.const, i8x16.shuffle
		if(subOp >= 21 && subOp <= 34) return SkipBytes(ptr, end, 1);             //lane extract/replace
		if(subOp >= 84 && subOp <= 91) return SkipMemArg(ptr, end) && SkipBytes(ptr, end, 1); //lane loads/stores
		if(subOp == 92 || subOp == 93) return SkipMemArg(ptr, end);              //load zero
		return subOp <= 0x113;                                                    //no immediates (incl. relaxed SIMD)
	}
	default:
		if(op >= 0x28 && op <= 0x3E) return SkipMemArg(ptr, end); //loads/stores
		if(op <= 0x01 || op == 0x05 || op == 0x0B || op == 0x0F || op == 0x1A || op == 0x1B) return true;
		if(op >= 0x45 && op <= 0xC4) return true; //numeric
		if(op == 0xD1) return true;               //ref.is_null
		return false;
	}
}

//Locals of the region function: [ctx param][REGION_LOCALS i32][i32 x max][i64 x max][f32 x max][v128 x max]
static const uint32 REGION_LOCALS = 1; //generation counter at entry
static const uint8 g_localTypes[4] = {Wasm::TYPE_I32, Wasm::TYPE_I64, Wasm::TYPE_F32, Wasm::TYPE_V128};

bool CWasmModuleBuilder::RewriteBodyForRegion(const uint8* body, uint32 bodySize, std::vector<uint8>& code, uint32 counts[4])
{
	const uint8* ptr = body;
	const uint8* end = body + bodySize;
	uint32 size = 0;
	if(!ReadULeb128(ptr, end, size) || (size != static_cast<uint32>(end - ptr))) return false;
	struct DECL
	{
		uint32 first, count, type, typeOffset;
	};
	std::vector<DECL> decls;
	uint32 declCount = 0;
	if(!ReadULeb128(ptr, end, declCount)) return false;
	for(uint32 i = 0; i < 4; i++) counts[i] = 0;
	uint32 next = 1;
	for(uint32 i = 0; i < declCount; i++)
	{
		uint32 count = 0;
		if(!ReadULeb128(ptr, end, count) || (ptr >= end)) return false;
		uint8 typeCode = *ptr++;
		uint32 type = 0;
		while((type < 4) && (g_localTypes[type] != typeCode)) type++;
		if(type == 4) return false;
		decls.push_back({next, count, type, counts[type]});
		counts[type] += count;
		next += count;
	}
	//Placeholder indexes: (type << 24) | index within the type; resolved by the caller
	code.clear();
	int32 depth = 0;
	while(ptr < end)
	{
		const uint8* instStart = ptr;
		uint8 op = *ptr++;
		if(op >= 0x20 && op <= 0x22)
		{
			uint32 index = 0;
			if(!ReadULeb128(ptr, end, index)) return false;
			uint32 mapped = 0xFFFFFFFF;
			if(index == 0)
			{
				mapped = 0;
			}
			else
			{
				for(const auto& decl : decls)
				{
					if(index >= decl.first && index < decl.first + decl.count)
					{
						mapped = 0x80000000 | (decl.type << 24) | (decl.typeOffset + index - decl.first);
					}
				}
				if(mapped == 0xFFFFFFFF) return false;
			}
			code.push_back(op);
			//Fixed 5-byte LEB, patched once the region's local layout is known
			for(uint32 i = 0; i < 4; i++) code.push_back(static_cast<uint8>(0x80 | ((mapped >> (7 * i)) & 0x7F)));
			code.push_back(static_cast<uint8>((mapped >> 28) & 0x0F));
			continue;
		}
		if(!SkipImmediates(op, ptr, end)) return false;
		if(op == 0x02 || op == 0x03 || op == 0x04) depth++;
		if(op == 0x0B)
		{
			if(depth == 0)
			{
				//End of the function: must be the last byte
				return ptr == end;
			}
			depth--;
		}
		code.insert(code.end(), instStart, ptr);
	}
	return false;
}

static bool BuildRegionBody(const std::vector<std::pair<const uint8*, uint32>>& bodies, const std::vector<uint32>& starts,
                            const CWasmModuleBuilder::REGION_PARAMS& params, Framework::CMemStream& out)
{
	uint32 count = static_cast<uint32>(bodies.size());
	if((count < 2) || (starts.size() != count)) return false;
	std::vector<std::vector<uint8>> codes(count);
	uint32 maxCounts[4] = {};
	for(uint32 i = 0; i < count; i++)
	{
		uint32 counts[4];
		if(!CWasmModuleBuilder::RewriteBodyForRegion(bodies[i].first, bodies[i].second, codes[i], counts)) return false;
		for(uint32 t = 0; t < 4; t++) maxCounts[t] = std::max(maxCounts[t], counts[t]);
		if((i != 0) && (starts[i] <= starts[i - 1])) return false;
		if(starts[i] & 3) return false;
	}
	uint32 regionStart = starts[0];
	uint32 entryCount = ((starts[count - 1] - regionStart) / 4) + 1;
	if(entryCount > 0x10000) return false;
	uint32 typeBase[4];
	typeBase[0] = 1 + REGION_LOCALS;
	typeBase[1] = typeBase[0] + maxCounts[0];
	typeBase[2] = typeBase[1] + maxCounts[1];
	typeBase[3] = typeBase[2] + maxCounts[2];
	Framework::CMemStream body;
	auto writeU = [&](uint64 v) { CWasmModuleBuilder::WriteULeb128(body, v); };
	auto writeS = [&](int64 v) { CWasmModuleBuilder::WriteSLeb128(body, v); };
	auto loadContext = [&](uint32 offset) {
		body.Write8(0x20); //local.get 0
		body.Write8(0x00);
		body.Write8(0x28); //i32.load
		body.Write8(0x02);
		writeU(offset);
	};
	//Locals
	{
		uint32 localCounts[4] = {REGION_LOCALS + maxCounts[0], maxCounts[1], maxCounts[2], maxCounts[3]};
		uint32 declCount = 0;
		for(uint32 t = 0; t < 4; t++)
			if(localCounts[t]) declCount++;
		writeU(declCount);
		for(uint32 t = 0; t < 4; t++)
		{
			if(!localCounts[t]) continue;
			writeU(localCounts[t]);
			body.Write8(g_localTypes[t]);
		}
	}
	//Generation at entry
	loadContext(params.generationOffset);
	body.Write8(0x21);
	body.Write8(0x01);
	body.Write8(0x03); //loop (dispatch)
	body.Write8(0x40);
	body.Write8(0x02); //block $exit
	body.Write8(0x40);
	for(uint32 i = 0; i < count; i++)
	{
		body.Write8(0x02); //block $b(count - 1) ... $b0
		body.Write8(0x40);
	}
	//(pc & mask) - start) >> 2 (wraps to a large value below the start: default)
	loadContext(params.pcOffset);
	body.Write8(0x41);
	writeS(static_cast<int32>(params.addressMask));
	body.Write8(0x71); //i32.and
	body.Write8(0x41);
	writeS(static_cast<int32>(regionStart));
	body.Write8(0x6B); //i32.sub
	body.Write8(0x41);
	writeS(2);
	body.Write8(0x76); //i32.shr_u
	body.Write8(0x0E); //br_table
	writeU(entryCount);
	{
		uint32 member = 0;
		for(uint32 e = 0; e < entryCount; e++)
		{
			uint32 address = regionStart + (e * 4);
			if((member < count) && (starts[member] == address))
			{
				writeU(member);
				member++;
			}
			else
			{
				writeU(count); //$exit
			}
		}
	}
	writeU(count); //default: $exit
	for(uint32 i = 0; i < count; i++)
	{
		body.Write8(0x0B); //end of $b(i): block i starts here
		body.Write8(0x02); //block (stands for the block function's body: a branch to its outermost label ends the block)
		body.Write8(0x40);
		{
			//Resolve local placeholders (minimal LEB in the output)
			const auto& code = codes[i];
			const uint8* p = code.data();
			const uint8* e = code.data() + code.size();
			const uint8* copied = p;
			while(p < e)
			{
				uint8 op = *p++;
				if(op >= 0x20 && op <= 0x22)
				{
					body.Write(copied, p - copied);
					uint32 value = 0;
					for(uint32 b = 0; b < 4; b++) value |= static_cast<uint32>(p[b] & 0x7F) << (7 * b);
					value |= static_cast<uint32>(p[4] & 0x0F) << 28;
					if(value & 0x80000000)
					{
						value = typeBase[(value >> 24) & 0x3] + (value & 0xFFFFFF);
					}
					writeU(value);
					p += 5;
					copied = p;
					continue;
				}
				if(!SkipImmediates(op, p, e)) return false;
			}
			body.Write(copied, e - copied);
		}
		body.Write8(0x0B); //end of the block body
		//Leave if the exception flag is set or a block was deleted, else dispatch again
		loadContext(params.exceptionOffset);
		loadContext(params.generationOffset);
		body.Write8(0x20);
		body.Write8(0x01);
		body.Write8(0x73); //i32.xor
		body.Write8(0x72); //i32.or
		body.Write8(0x0D); //br_if $exit
		writeU(count - 1 - i);
		if(i + 1 < count)
		{
			//Next block in address order (usually the fall-through successor): its code follows
			loadContext(params.pcOffset);
			body.Write8(0x41);
			writeS(static_cast<int32>(params.addressMask));
			body.Write8(0x71); //i32.and
			body.Write8(0x41);
			writeS(static_cast<int32>(starts[i + 1]));
			body.Write8(0x47); //i32.ne
			body.Write8(0x0D); //br_if $dispatch
			writeU(count - i);
		}
		else
		{
			body.Write8(0x0C); //br $dispatch
			writeU(count - i);
		}
	}
	body.Write8(0x0B); //end $exit
	body.Write8(0x0B); //end loop
	body.Write8(0x0B); //end function

	CWasmModuleBuilder::WriteULeb128(out, body.GetSize());
	out.Write(body.GetBuffer(), body.GetSize());
	return true;
}

std::vector<uint8> CWasmModuleBuilder::BuildBatchModule(const std::vector<MODULE_BYTES>& modules, const std::vector<BATCH_FUNCTION>& functions,
                                                        const std::vector<uint32>& exportFunctions, const REGION_PARAMS& regionParams)
{
	struct SECTIONS
	{
		const uint8* type = nullptr;
		uint32 typeSize = 0;
		uint32 importCount = 0;
		uint32 functionTypeIndex = 0;
		const uint8* body = nullptr; //function body, with its size prefix
		uint32 bodySize = 0;
		const uint8* name = nullptr;  //function name (no length prefix)
		uint32 nameSize = 0;
	};
	std::vector<SECTIONS> parsed;
	parsed.reserve(modules.size());
	uint32 largestTypeModule = 0;
	bool importsBlockTable = false;
	bool hasNames = false;
	for(const auto& module : modules)
	{
		SECTIONS sections;
		const uint8* ptr = module.data + 8;
		const uint8* end = module.data + module.size;
		if((module.size < 8) || (*reinterpret_cast<const uint32*>(module.data) != Wasm::BINARY_MAGIC)) return {};
		while(ptr < end)
		{
			uint8 id = *ptr++;
			uint32 size = 0;
			if(!ReadULeb128(ptr, end, size) || (size > static_cast<uint32>(end - ptr))) return {};
			const uint8* content = ptr;
			const uint8* contentEnd = ptr + size;
			switch(id)
			{
			case Wasm::SECTION_ID_TYPE:
				sections.type = content;
				sections.typeSize = size;
				break;
			case Wasm::SECTION_ID_IMPORT:
				sections.importCount = content[0];
				break;
			case Wasm::SECTION_ID_FUNCTION:
				if((size != 2) || (content[0] != 1)) return {};
				sections.functionTypeIndex = content[1];
				break;
			case Wasm::SECTION_ID_CODE:
			{
				const uint8* p = content;
				uint32 count = 0, bodySize = 0;
				if(!ReadULeb128(p, contentEnd, count) || (count != 1)) return {};
				const uint8* bodyStart = p;
				if(!ReadULeb128(p, contentEnd, bodySize) || (p + bodySize != contentEnd)) return {};
				sections.body = bodyStart;
				sections.bodySize = static_cast<uint32>(contentEnd - bodyStart);
			}
			break;
			case 0:
			{
				//"name" section: name length (4), "name", subsection 1, size, count 1, index 0, name
				const uint8* p = content;
				uint32 length = 0, subSize = 0, count = 0, index = 0, nameSize = 0;
				if(!ReadULeb128(p, contentEnd, length) || (length != 4) || (memcmp(p, "name", 4) != 0)) break;
				p += 4;
				if((p >= contentEnd) || (*p++ != 1)) break;
				if(!ReadULeb128(p, contentEnd, subSize) || !ReadULeb128(p, contentEnd, count) || (count != 1)) break;
				if(!ReadULeb128(p, contentEnd, index) || !ReadULeb128(p, contentEnd, nameSize) || (p + nameSize > contentEnd)) break;
				sections.name = p;
				sections.nameSize = nameSize;
			}
			break;
			default:
				break;
			}
			ptr = contentEnd;
		}
		if(!sections.type || !sections.body) return {};
		if(sections.importCount == 3) importsBlockTable = true;
		if(sections.name) hasNames = true;
		if((parsed.empty()) || (sections.typeSize > parsed[largestTypeModule].typeSize)) largestTypeModule = static_cast<uint32>(parsed.size());
		parsed.push_back(sections);
	}
	if(parsed.empty()) return {};

	//Canonical numbering: the longest type section contains all the others
	Framework::CMemStream stream;
	stream.Write32(Wasm::BINARY_MAGIC);
	stream.Write32(Wasm::BINARY_VERSION);
	WriteSectionHeader(stream, Wasm::SECTION_ID_TYPE, parsed[largestTypeModule].typeSize);
	stream.Write(parsed[largestTypeModule].type, parsed[largestTypeModule].typeSize);

	//Imports: same as WriteModule
	{
		CWasmModuleBuilder builder;
		builder.SetImportsBlockTable(importsBlockTable);
		Framework::CMemStream importStream;
		builder.WriteImportSection(importStream);
		stream.Write(importStream.GetBuffer(), importStream.GetSize());
	}

	uint32 count = static_cast<uint32>(functions.size());
	{
		Framework::CMemStream section;
		WriteULeb128(section, count);
		for(const auto& function : functions)
		{
			if(function.modules.empty() || function.modules[0] >= parsed.size()) return {};
			WriteULeb128(section, parsed[function.modules[0]].functionTypeIndex);
		}
		WriteSectionHeader(stream, Wasm::SECTION_ID_FUNCTION, section.GetSize());
		stream.Write(section.GetBuffer(), section.GetSize());
	}
	{
		Framework::CMemStream section;
		WriteULeb128(section, exportFunctions.size());
		for(uint32 i = 0; i < exportFunctions.size(); i++)
		{
			if(exportFunctions[i] >= count) return {};
			char name[16];
			snprintf(name, sizeof(name), "f%u", i);
			WriteName(section, name);
			section.Write8(Wasm::IMPORT_EXPORT_TYPE_FUNCTION);
			WriteULeb128(section, exportFunctions[i]);
		}
		WriteSectionHeader(stream, Wasm::SECTION_ID_EXPORT, section.GetSize());
		stream.Write(section.GetBuffer(), section.GetSize());
	}
	{
		Framework::CMemStream section;
		WriteULeb128(section, count);
		for(const auto& function : functions)
		{
			if(function.modules.size() == 1)
			{
				const auto& sections = parsed[function.modules[0]];
				section.Write(sections.body, sections.bodySize);
				continue;
			}
			std::vector<std::pair<const uint8*, uint32>> bodies;
			for(uint32 index : function.modules)
			{
				if(index >= parsed.size()) return {};
				//Blocks that chain to others (block table import) can't be part of a region
				if(parsed[index].importCount != 2) return {};
				bodies.push_back({parsed[index].body, parsed[index].bodySize});
			}
			if(!BuildRegionBody(bodies, function.starts, regionParams, section)) return {};
		}
		WriteSectionHeader(stream, Wasm::SECTION_ID_CODE, section.GetSize());
		stream.Write(section.GetBuffer(), section.GetSize());
	}
	if(hasNames)
	{
		Framework::CMemStream names;
		WriteULeb128(names, count);
		for(uint32 i = 0; i < count; i++)
		{
			const auto& sections = parsed[functions[i].modules[0]];
			WriteULeb128(names, i);
			WriteULeb128(names, sections.nameSize);
			if(sections.nameSize) names.Write(sections.name, sections.nameSize);
		}
		Framework::CMemStream section;
		WriteName(section, "name");
		section.Write8(1);
		WriteULeb128(section, names.GetSize());
		section.Write(names.GetBuffer(), names.GetSize());
		WriteSectionHeader(stream, 0, section.GetSize());
		stream.Write(section.GetBuffer(), section.GetSize());
	}
	return std::vector<uint8>(stream.GetBuffer(), stream.GetBuffer() + stream.GetSize());
}
