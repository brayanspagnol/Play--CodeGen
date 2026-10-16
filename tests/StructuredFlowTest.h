#pragma once

#include "Test.h"

//Control flow that BuildLabelFlows' patterns can't express (nested loops, an exit from an inner
//loop out of both loops, a diamond inside a loop), plus irreducible graph rejection on wasm.
class CStructuredFlowTest : public CTest
{
public:
	void Run() override;
	void Compile(Jitter::CJitter&) override;

private:
	struct CONTEXT
	{
		uint32 i;
		uint32 j;
		uint32 sum;
		uint32 odd;
		uint32 even;
		uint32 exitKind;
		uint32 breakAt;
	};

	void RunCase(uint32 breakAt);

	CONTEXT m_context;
	FunctionType m_function;
	bool m_irreducibleRejected = false;
};
