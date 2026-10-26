#pragma once

#include "Stream.h"
#include "Types.h"
#include <vector>
#include <string>

class CWasmModuleBuilder
{
public:
	struct FUNCTION_TYPE
	{
		std::vector<uint32> params;
		std::vector<uint32> results;
	};

	typedef std::vector<uint8> FunctionCode;
	//Code that can tail call the block at a guest address directly once it is in the same batch module
	//(CCodeGen_Wasm::Emit_ExternJmpLink). Offsets are relative to the function code (after the local declarations
	//in WriteModule's output, before them in the "play.link" section, which counts from the body like branch hints)
	struct LINK_SITE
	{
		static constexpr int32 UNLINKED = 0x3FFFFFFF; //linkBase + UNLINKED is never a table index
		uint32 target = 0;         //guest address (masked like the function index table)
		uint32 start = 0;          //site bytes [start, end)
		uint32 end = 0;
		uint32 indexOffset = 0;    //5-byte signed LEB128: batch function index k (UNLINKED: not linked)
		uint32 functionOffset = 0; //5-byte LEB128: function index called when linked
	};
	struct FUNCTION
	{
		FunctionCode code;
		uint32 localI32Count = 0;
		uint32 localI64Count = 0;
		uint32 localF32Count = 0;
		uint32 localV128Count = 0;
		std::string name; //Written to a "name" custom section when not empty (profiling)
		std::vector<std::pair<uint32, uint8>> branchHints; //Code offsets of hinted 'if's (1: likely, 0: unlikely), written to a branch hint section
		std::vector<LINK_SITE> linkSites; //Written to a "play.link" section; the module then imports the global env.linkBase
		uint32 linkAddress = ~0U;         //Guest address of the function's block (in the "play.link" section): link sites to it can be linked
	};

	static void WriteSLeb128(Framework::CStream&, int64);
	static void WriteULeb128(Framework::CStream&, uint64);

	static uint32 GetULeb128Size(uint32);
	//5-byte (s)LEB128, patchable in place; values must fit 31 bits signed
	static void WritePaddedLeb128(uint8*, int32);

	void AddFunctionType(FUNCTION_TYPE);
	void AddFunction(FUNCTION);
	//Import a second funcref table, env.blockTable (used by return_call_indirect ... table 1)
	void SetImportsBlockTable(bool);
	//Import the immutable i32 global env.linkBase (global 0, used by link sites)
	void SetImportsLinkBase(bool);

	void SetFunctionTypeIndex(uint32);
	void WriteModule(Framework::CStream&);

	//Builds one module that holds the functions of several single-function modules written by
	//WriteModule (canonical type numbering: bodies are copied unchanged). Function k is exported as
	//"f<k>". Returns an empty vector if a module has an unexpected layout.
	struct MODULE_BYTES
	{
		const uint8* data;
		uint32 size;
	};
	//Link sites to another function of the batch (by the guest addresses in "play.link" sections) are patched
	//to call it directly, the others are removed (see LINK_SITE). The batch imports env.linkBase if any module does.
	static std::vector<uint8> BuildBatchModule(const std::vector<MODULE_BYTES>&);

private:
	static void WriteSectionHeader(Framework::CStream&, uint8, uint32);
	void WriteImportSection(Framework::CStream&);
	void WriteImportEntries(Framework::CStream&);

	std::vector<FUNCTION_TYPE> m_functionTypes;
	std::vector<FUNCTION> m_functions;
	bool m_importsBlockTable = false;
	bool m_importsLinkBase = false;
	uint32 m_functionTypeIndex = 0;
};
