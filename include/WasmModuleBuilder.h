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
	struct FUNCTION
	{
		FunctionCode code;
		uint32 localI32Count = 0;
		uint32 localI64Count = 0;
		uint32 localF32Count = 0;
		uint32 localV128Count = 0;
		std::string name; //Written to a "name" custom section when not empty (profiling)
	};

	static void WriteSLeb128(Framework::CStream&, int64);
	static void WriteULeb128(Framework::CStream&, uint64);

	static uint32 GetULeb128Size(uint32);

	void AddFunctionType(FUNCTION_TYPE);
	void AddFunction(FUNCTION);
	//Import a second funcref table, env.blockTable (used by return_call_indirect ... table 1)
	void SetImportsBlockTable(bool);

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
	static std::vector<uint8> BuildBatchModule(const std::vector<MODULE_BYTES>&);

	//Regions: one function runs several blocks (single-function "vi" modules from WriteModule that
	//don't import the block table). Entry and every block exit dispatch on the guest PC through a
	//br_table: the next block runs in the same function if it belongs to the region and nothing
	//asked to leave (exception flag) and no block was deleted since the region was entered
	//(generation counter); otherwise the function returns. Same semantics as returning to a
	//dispatcher loop that checks the flag and looks the next block up.
	struct REGION_PARAMS
	{
		uint32 pcOffset = 0;         //context offsets (i32 values)
		uint32 exceptionOffset = 0;  //nonzero: return
		uint32 generationOffset = 0; //changes when blocks are deleted
		uint32 addressMask = ~0U;    //applied to the PC before the dispatch
	};
	struct BATCH_FUNCTION
	{
		std::vector<uint32> modules; //indexes of the input modules, one = copied unchanged
		std::vector<uint32> starts;  //guest start address of each block (regions), ascending
	};
	//Builds one module with one function per BATCH_FUNCTION. Export k ("f<k>") is the function of
	//exportFunctions[k] (a function can be exported several times). Empty vector on failure.
	static std::vector<uint8> BuildBatchModule(const std::vector<MODULE_BYTES>&, const std::vector<BATCH_FUNCTION>&,
	                                           const std::vector<uint32>& exportFunctions, const REGION_PARAMS&);
	//Rewrites the function body (size prefixed) of a module for a region (exposed for tests)
	static bool RewriteBodyForRegion(const uint8* body, uint32 bodySize, std::vector<uint8>& code, uint32 counts[4]);

private:
	static void WriteSectionHeader(Framework::CStream&, uint8, uint32);
	void WriteImportSection(Framework::CStream&);

	std::vector<FUNCTION_TYPE> m_functionTypes;
	std::vector<FUNCTION> m_functions;
	bool m_importsBlockTable = false;
	uint32 m_functionTypeIndex = 0;
};
