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

	void WriteModule(Framework::CStream&);

private:
	void WriteSectionHeader(Framework::CStream&, uint8, uint32);

	std::vector<FUNCTION_TYPE> m_functionTypes;
	std::vector<FUNCTION> m_functions;
	bool m_importsBlockTable = false;
};
