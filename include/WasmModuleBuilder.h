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

private:
	static void WriteSectionHeader(Framework::CStream&, uint8, uint32);
	void WriteImportSection(Framework::CStream&);

	std::vector<FUNCTION_TYPE> m_functionTypes;
	std::vector<FUNCTION> m_functions;
	bool m_importsBlockTable = false;
	uint32 m_functionTypeIndex = 0;
};
