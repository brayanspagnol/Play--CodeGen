#pragma once

#include <memory>
#include <vector>
#include "Types.h"

namespace Jitter
{
	//Runs a function straight from the Jitter's statements (before optimization and code generation), with
	//the semantics of the WebAssembly code generator: same operations, same helper calls, same results. Lets a
	//caller run new code at once while the optimizer and the wasm compile happen elsewhere (CJitter::End on
	//another thread). Built by CJitter::BuildInterpreterProgram; immutable, so it can be shared and kept alive
	//while it runs.
	class CInterpreterProgram
	{
	public:
		enum OPERAND_KIND : uint8
		{
			OPERAND_NONE,
			OPERAND_CONTEXT,
			OPERAND_CONSTANT,   //32 bits (constants and constant pointers)
			OPERAND_CONSTANT64, //value | (valueHi << 32)
			OPERAND_RELATIVE,   //context + value, any size
			OPERAND_TEMPORARY,  //temporary area + value, any size
			OPERAND_RELREF,     //32-bit pointer stored at context + value
		};

		struct OPERAND
		{
			OPERAND_KIND kind = OPERAND_NONE;
			uint8 size = 0; //bytes (symbol size)
			uint32 value = 0;
			uint32 valueHi = 0;
		};

		struct INSTRUCTION
		{
			uint16 op = 0;
			uint8 condition = 0; //jump/compare condition, page access size, load/store scale or mask
			uint8 signature = 0; //OP_CALL: CALL_SIGNATURE
			OPERAND dst, src1, src2, src3;
			uint32 target = 0; //jumps: instruction index
		};

		enum CALL_SIGNATURE : uint8
		{
			SIG_VI,
			SIG_III,
			SIG_JII,
			SIG_VIII,
			SIG_VIJI,
			SIG_IIII,
			SIG_JIJI,
			SIG_VII,
			SIG_II,
			SIG_JI,
			SIG_VIIII,
			SIG_IIIII,
			SIG_MAX
		};
		static bool GetCallSignature(const char*, CALL_SIGNATURE&);

		void Run(void* context) const;

		std::vector<INSTRUCTION> m_instructions;
		uint32 m_temporaryBytes = 0;
	};

	typedef std::shared_ptr<const CInterpreterProgram> InterpreterProgramPtr;
}
