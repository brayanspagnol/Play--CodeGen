#include "StructuredFlowTest.h"
#include "MemStream.h"
#include <stdexcept>
#ifdef __EMSCRIPTEN__
#include "Jitter_CodeGen_Wasm.h"
#endif

#define OUTER_COUNT (5)
#define INNER_COUNT (7)

void CStructuredFlowTest::RunCase(uint32 breakAt)
{
	//Reference
	uint32 sum = 0, odd = 0, even = 0, exitKind = 0, i = 0, j = 0;
	for(i = 0; i != OUTER_COUNT; i++)
	{
		for(j = 0; j != INNER_COUNT; j++)
		{
			if(sum == breakAt)
			{
				exitKind = 2;
				goto out;
			}
			if(j & 1)
				odd += j;
			else
				even += i;
			sum++;
		}
	}
	exitKind = 1;
out:

	memset(&m_context, 0, sizeof(CONTEXT));
	m_context.breakAt = breakAt;
	m_function(&m_context);
	TEST_VERIFY(m_context.i == i);
	TEST_VERIFY(m_context.j == j);
	TEST_VERIFY(m_context.sum == sum);
	TEST_VERIFY(m_context.odd == odd);
	TEST_VERIFY(m_context.even == even);
	TEST_VERIFY(m_context.exitKind == exitKind);
}

void CStructuredFlowTest::Run()
{
	RunCase(~0U);
	RunCase(17);
	RunCase(0);
#ifdef __EMSCRIPTEN__
	TEST_VERIFY(m_irreducibleRejected);
#endif
}

void CStructuredFlowTest::Compile(Jitter::CJitter& jitter)
{
#ifdef __EMSCRIPTEN__
	auto wasmCodeGen = static_cast<Jitter::CCodeGen_Wasm*>(jitter.GetCodeGen());
	bool wasStructured = wasmCodeGen->GetStructuredControlFlow();
	wasmCodeGen->SetStructuredControlFlow(true);
#endif
	auto increment = [&](size_t offset, uint32 amount) {
		jitter.PushRel(offset);
		jitter.PushCst(amount);
		jitter.Add();
		jitter.PullRel(offset);
	};

	{
		Framework::CMemStream codeStream;
		jitter.SetStream(&codeStream);
		jitter.Begin();
		{
			auto outerLabel = jitter.CreateLabel();
			auto innerLabel = jitter.CreateLabel();
			auto innerEndLabel = jitter.CreateLabel();
			auto doneLabel = jitter.CreateLabel();
			auto outLabel = jitter.CreateLabel();

			jitter.PushCst(0);
			jitter.PullRel(offsetof(CONTEXT, i));

			jitter.MarkLabel(outerLabel);
			jitter.PushRel(offsetof(CONTEXT, i));
			jitter.PushCst(OUTER_COUNT);
			jitter.BeginIf(Jitter::CONDITION_EQ);
			jitter.Goto(doneLabel);
			jitter.EndIf();

			jitter.PushCst(0);
			jitter.PullRel(offsetof(CONTEXT, j));

			jitter.MarkLabel(innerLabel);
			jitter.PushRel(offsetof(CONTEXT, j));
			jitter.PushCst(INNER_COUNT);
			jitter.BeginIf(Jitter::CONDITION_EQ);
			jitter.Goto(innerEndLabel);
			jitter.EndIf();

			//Exit out of both loops
			jitter.PushRel(offsetof(CONTEXT, sum));
			jitter.PushRel(offsetof(CONTEXT, breakAt));
			jitter.BeginIf(Jitter::CONDITION_EQ);
			{
				jitter.PushCst(2);
				jitter.PullRel(offsetof(CONTEXT, exitKind));
				jitter.Goto(outLabel);
			}
			jitter.EndIf();

			//Diamond
			jitter.PushRel(offsetof(CONTEXT, j));
			jitter.PushCst(1);
			jitter.And();
			jitter.PushCst(0);
			jitter.BeginIf(Jitter::CONDITION_NE);
			{
				jitter.PushRel(offsetof(CONTEXT, odd));
				jitter.PushRel(offsetof(CONTEXT, j));
				jitter.Add();
				jitter.PullRel(offsetof(CONTEXT, odd));
			}
			jitter.Else();
			{
				jitter.PushRel(offsetof(CONTEXT, even));
				jitter.PushRel(offsetof(CONTEXT, i));
				jitter.Add();
				jitter.PullRel(offsetof(CONTEXT, even));
			}
			jitter.EndIf();

			increment(offsetof(CONTEXT, sum), 1);
			increment(offsetof(CONTEXT, j), 1);
			jitter.Goto(innerLabel);

			jitter.MarkLabel(innerEndLabel);
			increment(offsetof(CONTEXT, i), 1);
			jitter.Goto(outerLabel);

			jitter.MarkLabel(doneLabel);
			jitter.PushCst(1);
			jitter.PullRel(offsetof(CONTEXT, exitKind));

			jitter.MarkLabel(outLabel);
		}
		jitter.End();
		m_function = FunctionType(codeStream.GetBuffer(), codeStream.GetSize());
	}

#ifdef __EMSCRIPTEN__
	//Loop with two entries: if(sum != 0) goto b; a: i++; b: j++; if(j != 3) goto a;
	{
		Framework::CMemStream codeStream;
		jitter.SetStream(&codeStream);
		jitter.Begin();
		{
			auto aLabel = jitter.CreateLabel();
			auto bLabel = jitter.CreateLabel();

			jitter.PushRel(offsetof(CONTEXT, sum));
			jitter.PushCst(0);
			jitter.BeginIf(Jitter::CONDITION_NE);
			jitter.Goto(bLabel);
			jitter.EndIf();

			jitter.MarkLabel(aLabel);
			increment(offsetof(CONTEXT, i), 1);

			jitter.MarkLabel(bLabel);
			increment(offsetof(CONTEXT, j), 1);
			jitter.PushRel(offsetof(CONTEXT, j));
			jitter.PushCst(3);
			jitter.BeginIf(Jitter::CONDITION_NE);
			jitter.Goto(aLabel);
			jitter.EndIf();
		}
		try
		{
			jitter.End();
		}
		catch(const std::runtime_error&)
		{
			m_irreducibleRejected = true;
		}
	}
	wasmCodeGen->SetStructuredControlFlow(wasStructured);
#endif
}
