#include "WasmModuleBuilder.h"
#include <cassert>
#include <cstring>
#include "WasmDefs.h"
#include "MemStream.h"
#include <cstdio>

static const char* BRANCH_HINT_SECTION = "metadata.code.branch_hint";
static const char* LINK_SECTION = "play.link";

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

void CWasmModuleBuilder::WritePaddedLeb128(uint8* bytes, int32 value)
{
	assert((value >= -0x40000000) && (value < 0x40000000));
	for(uint32 i = 0; i < 4; i++)
	{
		bytes[i] = (value & 0x7F) | 0x80;
		value >>= 7;
	}
	bytes[4] = value & 0x7F;
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

void CWasmModuleBuilder::SetImportsLinkBase(bool importsLinkBase)
{
	m_importsLinkBase = importsLinkBase;
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

	m_importsLinkBase = !m_functions[0].linkSites.empty();
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

	//Custom section "metadata.code.branch_hint" (must come before the code section): offsets are relative to
	//the start of the function body (its local declarations)
	if(!m_functions[0].branchHints.empty())
	{
		const auto& function = m_functions[0];
		uint32 localDeclSize = 1;
		for(uint32 count : {function.localI32Count, function.localI64Count, function.localF32Count, function.localV128Count})
		{
			if(count != 0) localDeclSize += GetULeb128Size(count) + 1;
		}
		Framework::CMemStream section;
		WriteName(section, BRANCH_HINT_SECTION);
		section.Write8(1); //Functions
		section.Write8(0); //Function index
		WriteULeb128(section, function.branchHints.size());
		for(const auto& hint : function.branchHints)
		{
			WriteULeb128(section, hint.first + localDeclSize);
			section.Write8(1); //Hint size
			section.Write8(hint.second);
		}
		WriteSectionHeader(stream, 0, section.GetSize());
		stream.Write(section.GetBuffer(), section.GetSize());
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

	//Custom section "play.link": guest address of function 0's block, then its link sites, offsets from the start of
	//its body like branch hints
	if(!m_functions[0].linkSites.empty() || (m_functions[0].linkAddress != ~0U))
	{
		const auto& function = m_functions[0];
		uint32 localDeclSize = 1;
		for(uint32 count : {function.localI32Count, function.localI64Count, function.localF32Count, function.localV128Count})
		{
			if(count != 0) localDeclSize += GetULeb128Size(count) + 1;
		}
		Framework::CMemStream section;
		WriteName(section, LINK_SECTION);
		WriteULeb128(section, function.linkAddress);
		WriteULeb128(section, function.linkSites.size());
		for(const auto& site : function.linkSites)
		{
			WriteULeb128(section, site.target);
			WriteULeb128(section, site.start + localDeclSize);
			WriteULeb128(section, site.end + localDeclSize);
			WriteULeb128(section, site.indexOffset + localDeclSize);
			WriteULeb128(section, site.functionOffset + localDeclSize);
		}
		WriteSectionHeader(stream, 0, section.GetSize());
		stream.Write(section.GetBuffer(), section.GetSize());
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
	Framework::CMemStream section;
	WriteImportEntries(section);
	WriteSectionHeader(stream, Wasm::SECTION_ID_IMPORT, section.GetSize());
	stream.Write(section.GetBuffer(), section.GetSize());
}

void CWasmModuleBuilder::WriteImportEntries(Framework::CStream& stream)
{
	stream.Write8(2 + (m_importsBlockTable ? 1 : 0) + (m_importsLinkBase ? 1 : 0)); //Import vector size

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

	if(m_importsLinkBase)
	{
		WriteName(stream, "env");
		WriteName(stream, "linkBase");
		stream.Write8(Wasm::IMPORT_EXPORT_TYPE_GLOBAL);
		stream.Write8(Wasm::TYPE_I32);
		stream.Write8(0x00); //Immutable
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

//Import section entries: tells which optional imports (env.blockTable, env.linkBase) a module has
static bool ParseImports(const uint8* p, const uint8* end, bool& importsBlockTable, bool& importsLinkBase)
{
	uint32 count = 0;
	if(!ReadULeb128(p, end, count)) return false;
	for(uint32 i = 0; i < count; i++)
	{
		uint32 length = 0, value = 0;
		if(!ReadULeb128(p, end, length) || (length > static_cast<uint32>(end - p))) return false;
		p += length;
		if(!ReadULeb128(p, end, length) || (length > static_cast<uint32>(end - p))) return false;
		const uint8* field = p;
		p += length;
		if(p + 2 > end) return false;
		uint8 kind = *p++;
		auto isField = [&](const char* name) { return (strlen(name) == length) && (memcmp(field, name, length) == 0); };
		switch(kind)
		{
		case Wasm::IMPORT_EXPORT_TYPE_FUNCTION:
			if(!ReadULeb128(p, end, value)) return false;
			break;
		case Wasm::IMPORT_EXPORT_TYPE_TABLE:
		{
			p++; //Reftype
			if(p >= end) return false;
			uint8 flags = *p++;
			if(!ReadULeb128(p, end, value)) return false;
			if((flags & 1) && !ReadULeb128(p, end, value)) return false;
			if(isField("blockTable")) importsBlockTable = true;
		}
		break;
		case Wasm::IMPORT_EXPORT_TYPE_MEMORY:
		{
			uint8 flags = *p++;
			if(!ReadULeb128(p, end, value)) return false;
			if((flags & 1) && !ReadULeb128(p, end, value)) return false;
		}
		break;
		case Wasm::IMPORT_EXPORT_TYPE_GLOBAL:
			p += 2; //Value type, mutability
			if(p > end) return false;
			if(isField("linkBase")) importsLinkBase = true;
			break;
		default:
			return false;
		}
	}
	return true;
}

uint32 CWasmModuleBuilder::GetLinkAddress(const MODULE_BYTES& module)
{
	if(module.size < 8) return ~0U;
	const uint8* ptr = module.data + 8;
	const uint8* end = module.data + module.size;
	uint32 linkNameSize = static_cast<uint32>(strlen(LINK_SECTION));
	while(ptr < end)
	{
		uint8 id = *ptr++;
		uint32 size = 0;
		if(!ReadULeb128(ptr, end, size) || (size > static_cast<uint32>(end - ptr))) return ~0U;
		const uint8* p = ptr;
		ptr += size;
		uint32 length = 0, address = 0;
		if((id != 0) || !ReadULeb128(p, ptr, length) || (length != linkNameSize) || (p + length > ptr) || (memcmp(p, LINK_SECTION, length) != 0)) continue;
		p += length;
		return ReadULeb128(p, ptr, address) ? address : ~0U;
	}
	return ~0U;
}

std::vector<uint8> CWasmModuleBuilder::BuildBatchModule(const std::vector<MODULE_BYTES>& modules)
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
		const uint8* hints = nullptr; //branch hints of function 0 (count and entries)
		uint32 hintsSize = 0;
		std::vector<LINK_SITE> linkSites; //offsets from the start of the body (after its size prefix)
		uint32 linkAddress = ~0U;
	};
	std::vector<SECTIONS> parsed;
	parsed.reserve(modules.size());
	uint32 largestTypeModule = 0;
	bool importsBlockTable = false;
	bool importsLinkBase = false;
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
				if(!ParseImports(content, contentEnd, importsBlockTable, importsLinkBase)) return {};
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
				//Branch hints: name, count 1, index 0, then the function's hint vector
				{
					const uint8* p = content;
					uint32 length = 0, count = 0, index = 0;
					uint32 hintNameSize = static_cast<uint32>(strlen(BRANCH_HINT_SECTION));
					if(ReadULeb128(p, contentEnd, length) && (length == hintNameSize) && (p + length <= contentEnd) && (memcmp(p, BRANCH_HINT_SECTION, length) == 0))
					{
						p += length;
						if(!ReadULeb128(p, contentEnd, count) || (count != 1) || !ReadULeb128(p, contentEnd, index) || (index != 0)) return {};
						sections.hints = p;
						sections.hintsSize = static_cast<uint32>(contentEnd - p);
						break;
					}
				}
				//Link sites: name, own address, count, (target, start, end, index offset, function offset) per site
				{
					const uint8* p = content;
					uint32 length = 0, count = 0;
					uint32 linkNameSize = static_cast<uint32>(strlen(LINK_SECTION));
					if(ReadULeb128(p, contentEnd, length) && (length == linkNameSize) && (p + length <= contentEnd) && (memcmp(p, LINK_SECTION, length) == 0))
					{
						p += length;
						if(!ReadULeb128(p, contentEnd, sections.linkAddress) || !ReadULeb128(p, contentEnd, count)) return {};
						for(uint32 i = 0; i < count; i++)
						{
							LINK_SITE site;
							if(!ReadULeb128(p, contentEnd, site.target) || !ReadULeb128(p, contentEnd, site.start) || !ReadULeb128(p, contentEnd, site.end) ||
							   !ReadULeb128(p, contentEnd, site.indexOffset) || !ReadULeb128(p, contentEnd, site.functionOffset)) return {};
							sections.linkSites.push_back(site);
						}
						break;
					}
				}
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
		builder.SetImportsLinkBase(importsLinkBase);
		Framework::CMemStream importStream;
		builder.WriteImportSection(importStream);
		stream.Write(importStream.GetBuffer(), importStream.GetSize());
	}

	uint32 count = static_cast<uint32>(parsed.size());
	{
		Framework::CMemStream section;
		WriteULeb128(section, count);
		for(const auto& sections : parsed) WriteULeb128(section, sections.functionTypeIndex);
		WriteSectionHeader(stream, Wasm::SECTION_ID_FUNCTION, section.GetSize());
		stream.Write(section.GetBuffer(), section.GetSize());
	}
	{
		Framework::CMemStream section;
		WriteULeb128(section, count);
		for(uint32 i = 0; i < count; i++)
		{
			char name[16];
			snprintf(name, sizeof(name), "f%u", i);
			WriteName(section, name);
			section.Write8(Wasm::IMPORT_EXPORT_TYPE_FUNCTION);
			WriteULeb128(section, i);
		}
		WriteSectionHeader(stream, Wasm::SECTION_ID_EXPORT, section.GetSize());
		stream.Write(section.GetBuffer(), section.GetSize());
	}
	{
		uint32 hintedCount = 0;
		for(const auto& sections : parsed) hintedCount += sections.hints ? 1 : 0;
		if(hintedCount != 0)
		{
			Framework::CMemStream section;
			WriteName(section, BRANCH_HINT_SECTION);
			WriteULeb128(section, hintedCount);
			for(uint32 i = 0; i < count; i++)
			{
				if(!parsed[i].hints) continue;
				WriteULeb128(section, i);
				section.Write(parsed[i].hints, parsed[i].hintsSize);
			}
			WriteSectionHeader(stream, 0, section.GetSize());
			stream.Write(section.GetBuffer(), section.GetSize());
		}
	}
	{
		Framework::CMemStream section;
		WriteULeb128(section, count);
		std::vector<uint8> body;
		for(const auto& sections : parsed)
		{
			if(sections.linkSites.empty())
			{
				section.Write(sections.body, sections.bodySize);
				continue;
			}
			//Link sites to another function of this batch become direct calls; the others are not needed
			body.assign(sections.body, sections.body + sections.bodySize);
			const uint8* p = sections.body;
			uint32 bodySize = 0;
			ReadULeb128(p, sections.body + sections.bodySize, bodySize);
			uint8* code = body.data() + (p - sections.body);
			for(const auto& site : sections.linkSites)
			{
				if((site.end > bodySize) || (site.start >= site.end) || (site.indexOffset + 5 > site.end) || (site.functionOffset + 5 > site.end)) return {};
				int32 linked = -1;
				for(uint32 i = 0; i < count; i++)
				{
					if(parsed[i].linkAddress == site.target) linked = static_cast<int32>(i);
				}
				if(linked < 0)
				{
					memset(code + site.start, 0x01, site.end - site.start); //nop
					continue;
				}
				WritePaddedLeb128(code + site.indexOffset, linked);
				WritePaddedLeb128(code + site.functionOffset, linked);
			}
			section.Write(body.data(), body.size());
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
			WriteULeb128(names, i);
			WriteULeb128(names, parsed[i].nameSize);
			if(parsed[i].nameSize) names.Write(parsed[i].name, parsed[i].nameSize);
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
