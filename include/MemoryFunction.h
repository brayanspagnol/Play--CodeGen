#pragma once

#include "Types.h"
#include <vector>

#if defined(__EMSCRIPTEN__)
#include <emscripten/bind.h>
#endif

class CMemoryFunction
{
public:
	CMemoryFunction();
	CMemoryFunction(const void*, size_t);
	CMemoryFunction(const CMemoryFunction&) = delete;
	CMemoryFunction(CMemoryFunction&&);

	virtual ~CMemoryFunction();

	bool IsEmpty() const;

	CMemoryFunction& operator=(const CMemoryFunction&) = delete;

	CMemoryFunction& operator=(CMemoryFunction&&);
	void operator()(void*);

	void* GetCode() const;
	size_t GetSize() const;

	void BeginModify();
	void EndModify();

	CMemoryFunction CreateInstance();

#if defined(__EMSCRIPTEN__)
	//Instantiates a module holding several functions (exports "f0".."f<count-1>", see
	//CWasmModuleBuilder::BuildBatchModule) and returns their function table indexes (empty on failure)
	static std::vector<int> CreateWasmBatch(const void*, size_t, uint32 count);
	//Calls fctId (a function from CreateWasmBatch, now owned by this object) instead of the current function
	void ReplaceWasmFunction(int fctId);
#endif

private:
	void ClearCache();
	void Reset();

	void* m_code;
	size_t m_size;
#if defined(__EMSCRIPTEN__)
	emscripten::val m_wasmModule;
#endif
};
