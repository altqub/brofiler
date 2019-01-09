#if _WIN32
#include "SymbolEngine.h"

#if BRO_ENABLE_SYMENGINE
#include <DbgHelp.h>
#pragma comment( lib, "DbgHelp.Lib" )

#include "Serialization.h"

namespace Brofiler
{
////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
//void ReportLastError()
//{
//	LPVOID lpMsgBuf;
//	DWORD dw = GetLastError();
//
//	FormatMessage(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
//								NULL, dw, MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT), 
//								(LPTSTR)&lpMsgBuf, 0, NULL);
//
//	MessageBox(NULL, (LPCTSTR)lpMsgBuf, TEXT("Error"), MB_OK);
//	LocalFree(lpMsgBuf);
//}
////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
typedef std::array<uintptr_t, 512> CallStackBuffer;

////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
class SymEngine : public SymbolEngine
{
	HANDLE hProcess;

	bool isInitialized;

	bool needRestorePreviousSettings;
	uint32 previousOptions;
	static const size_t MAX_SEARCH_PATH_LENGTH = 2048;
	char previousSearchPath[MAX_SEARCH_PATH_LENGTH];

	void InitSystemModules();
public:
	SymEngine();
	~SymEngine();

	void Init();
	void Close();

	// Get Symbol from PDB file
	virtual const Symbol * const GetSymbol(uint64 dwAddress) override;
};
////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////


SymEngine::SymEngine() : isInitialized(false), hProcess(GetCurrentProcess()), needRestorePreviousSettings(false), previousOptions(0)
{
}
////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
SymEngine::~SymEngine()
{
	Close();
}
////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
const Symbol * const SymEngine::GetSymbol(uint64 address)
{
	if (address == 0)
		return nullptr;

	Init();

	Symbol& symbol = cache[address];

	if (symbol.address != 0)
		return &symbol;

	if (!isInitialized)
		return nullptr;

	symbol.address = address;

	DWORD64 dwAddress = static_cast<DWORD64>(address);

	// Module Name
	IMAGEHLP_MODULEW64 moduleInfo;
	memset(&moduleInfo, 0, sizeof(IMAGEHLP_MODULEW64));
	moduleInfo.SizeOfStruct = sizeof(moduleInfo);
	if (SymGetModuleInfoW64(hProcess, dwAddress, &moduleInfo))
	{
		symbol.module = moduleInfo.ImageName;
	}


	// FileName and Line
	IMAGEHLP_LINEW64 lineInfo;
	memset(&lineInfo, 0, sizeof(IMAGEHLP_LINEW64));
	lineInfo.SizeOfStruct = sizeof(lineInfo);
	DWORD dwDisp;
	if (SymGetLineFromAddrW64(hProcess, dwAddress, &dwDisp, &lineInfo))
	{
		symbol.file = lineInfo.FileName;
		symbol.line = lineInfo.LineNumber;
	}

	const size_t length = (sizeof(SYMBOL_INFOW) + MAX_SYM_NAME*sizeof(WCHAR) + sizeof(ULONG64) - 1) / sizeof(ULONG64) + 1;

	// Function Name
	ULONG64 buffer[length];
	PSYMBOL_INFOW dbgSymbol = (PSYMBOL_INFOW)buffer;
	memset(dbgSymbol, 0, sizeof(buffer));
	dbgSymbol->SizeOfStruct = sizeof(SYMBOL_INFOW);
	dbgSymbol->MaxNameLen = MAX_SYM_NAME;

	DWORD64 offset = 0;
	if (SymFromAddrW(hProcess, dwAddress, &offset, dbgSymbol))
	{
		symbol.function.resize(dbgSymbol->NameLen);
		memcpy(&symbol.function[0], &dbgSymbol->Name[0], sizeof(WCHAR) * dbgSymbol->NameLen);
	}

	symbol.offset = static_cast<uintptr_t>(offset);

	return &symbol;
}
////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
// const char* USER_SYMBOL_SEARCH_PATH = "http://msdl.microsoft.com/download/symbols";
////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
void SymEngine::Init()
{
	if (!isInitialized)
	{
		previousOptions = SymGetOptions();

		memset(previousSearchPath, 0, MAX_SEARCH_PATH_LENGTH);
		SymGetSearchPath(hProcess, previousSearchPath, MAX_SEARCH_PATH_LENGTH);

		SymSetOptions(SymGetOptions() | SYMOPT_LOAD_LINES | SYMOPT_DEFERRED_LOADS | SYMOPT_UNDNAME | SYMOPT_INCLUDE_32BIT_MODULES | SYMOPT_LOAD_ANYTHING);
		if (!SymInitialize(hProcess, NULL, TRUE))
		{
			needRestorePreviousSettings = true;
			SymCleanup(hProcess);

			if (SymInitialize(hProcess, NULL, TRUE))
				isInitialized = true;
		}
		else
		{
			InitSystemModules();

			isInitialized = true;
		}
	}
}
////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
typedef DWORD(__stdcall *pZwQuerySystemInformation)(DWORD, LPVOID, DWORD, DWORD*);
#define SystemModuleInformation 11 // SYSTEMINFOCLASS
#define MAXIMUM_FILENAME_LENGTH 256

struct SYSTEM_MODULE_INFORMATION
{
	DWORD reserved1;
	DWORD reserved2;
	PVOID mappedBase;
	PVOID imageBase;
	DWORD imageSize;
	DWORD flags;
	WORD loadOrderIndex;
	WORD initOrderIndex;
	WORD loadCount;
	WORD moduleNameOffset;
	CHAR imageName[MAXIMUM_FILENAME_LENGTH];
};

#pragma warning (push)
#pragma warning(disable : 4200)
struct MODULE_LIST
{
	DWORD dwModules;
	SYSTEM_MODULE_INFORMATION pModulesInfo[];
};
#pragma warning (pop)

void SymEngine::InitSystemModules()
{
	ULONG returnLength = 0;
	ULONG systemInformationLength = 0;
	MODULE_LIST* pModuleList = nullptr;

	pZwQuerySystemInformation ZwQuerySystemInformation = (pZwQuerySystemInformation)GetProcAddress(GetModuleHandle(TEXT("ntdll.dll")), "ZwQuerySystemInformation");
	ZwQuerySystemInformation(SystemModuleInformation, pModuleList, systemInformationLength, &returnLength);
	systemInformationLength = returnLength;
	pModuleList = (MODULE_LIST*)Memory::Alloc(systemInformationLength);
	DWORD status = ZwQuerySystemInformation(SystemModuleInformation, pModuleList, systemInformationLength, &returnLength);
	if (status == ERROR_SUCCESS)
	{
		char systemRootPath[MAXIMUM_FILENAME_LENGTH] = { 0 };
		ExpandEnvironmentStringsA("%SystemRoot%", systemRootPath, MAXIMUM_FILENAME_LENGTH);

		const char* systemRootPattern = "\\SystemRoot";

		for (uint32_t i = 0; i < pModuleList->dwModules; ++i)
		{
			SYSTEM_MODULE_INFORMATION& module = pModuleList->pModulesInfo[i];

			if (strstr(module.imageName, systemRootPattern) == module.imageName)
			{
				char expandedPath[MAXIMUM_FILENAME_LENGTH] = { 0 };
				strcpy_s(expandedPath, systemRootPath);
				strcat_s(expandedPath, module.imageName + strlen(systemRootPattern));
				SymLoadModule64(hProcess, NULL, expandedPath, NULL, (DWORD64)module.imageBase, module.imageSize);
			}
			else
			{
				SymLoadModule64(hProcess, NULL, module.imageName, NULL, (DWORD64)module.imageBase, module.imageSize);
			}
		}
	}
	else
	{
		BRO_FAILED("Can't query System Module Information!");
	}

	if (pModuleList)
	{
		Memory::Free(pModuleList);
	}
}
////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
void SymEngine::Close()
{
	if (isInitialized)
	{
		SymCleanup(hProcess);
		isInitialized = false;
	}

	if (needRestorePreviousSettings)
	{
		HANDLE currentProcess = GetCurrentProcess();

		SymSetOptions(previousOptions);
		SymSetSearchPath(currentProcess, previousSearchPath);
		SymInitialize(currentProcess, NULL, TRUE);

		needRestorePreviousSettings = false;
	}
}


//////////////////////////////////////////////////////////////////////////
SymbolEngine* SymbolEngine::Get()
{
	static SymEngine pdbSymbolEngine;
	return &pdbSymbolEngine;
}

}
#endif //BRO_ENABLE_SYMENGINE
#endif //_WIN32