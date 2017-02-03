/*
MIT License

Copyright (c) 2016 Michael Fabian Dirks

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.
*/

//////////////////////////////////////////////////////////////////////////
// Includes
//////////////////////////////////////////////////////////////////////////
#include <vector>
#include <mutex>

#include "amf.h"
#include "windows.h"

// AMD AMF SDK
#include "components\Component.h"
#include "components\ComponentCaps.h"
#include "components\VideoEncoderVCE.h"

//////////////////////////////////////////////////////////////////////////
// Code
//////////////////////////////////////////////////////////////////////////
using namespace Plugin::AMD;

class CustomWriter : public amf::AMFTraceWriter {
	public:

	virtual void Write(const wchar_t* scope, const wchar_t* message) override {
		const wchar_t* realmsg = &(message[(33 + wcslen(scope) + 2)]); // Skip Time & Scope
		size_t msgLen = wcslen(realmsg) - (sizeof(wchar_t));

		blog(LOG_INFO, "[AMF Encoder] [%.*ls][%ls] %.*ls",
			12, &(message[11]),
			scope,
			msgLen, realmsg);
	}

	virtual void Flush() override {}
};

#pragma region Singleton
static AMF* __instance;
static std::mutex __instance_mutex;
void Plugin::AMD::AMF::Initialize() {
	const std::lock_guard<std::mutex> lock(__instance_mutex);
	if (!__instance)
		__instance = new AMF();
}

AMF* Plugin::AMD::AMF::Instance() {
	const std::lock_guard<std::mutex> lock(__instance_mutex);
	return __instance;
}

void Plugin::AMD::AMF::Finalize() {
	const std::lock_guard<std::mutex> lock(__instance_mutex);
	if (__instance)
		delete __instance;
	__instance = nullptr;
}
#pragma endregion Singleton

Plugin::AMD::AMF::AMF() {
	AMF_RESULT res = AMF_OK;

	#pragma region Null Class Members
	m_TimerPeriod = 0;
	m_AMFVersion_Plugin = AMF_FULL_VERSION;
	m_AMFVersion_Runtime = 0;
	m_AMFModule = 0;

	m_AMFFactory = nullptr;
	m_AMFTrace = nullptr;
	m_AMFDebug = nullptr;
	AMFQueryVersion = nullptr;
	AMFInit = nullptr;
	#pragma endregion Null Class Members

	#ifdef _WIN32
	void* pProductVersion = nullptr;
	uint32_t lProductVersionSize = 0;
	#endif

	// Initialize AMF Library
	AMF_LOG_DEBUG("<" __FUNCTION_NAME__ "> Initializing...");

	// Load AMF Runtime Library
	m_AMFModule = LoadLibraryW(AMF_DLL_NAME);
	if (!m_AMFModule) {
		QUICK_FORMAT_MESSAGE(msg, "Unable to load '%ls', error code %ld.",
			AMF_DLL_NAME,
			GetLastError());
		throw std::exception(msg.data());
	} else {
		AMF_LOG_DEBUG("<" __FUNCTION_NAME__ "> Loaded '%ls'.", AMF_DLL_NAME);
	}

	// Windows: Get Product Version for Driver Matching
	#ifdef _WIN32 
	{
		std::vector<char> verbuf(GetFileVersionInfoSizeW(AMF_DLL_NAME, nullptr));
		GetFileVersionInfoW(AMF_DLL_NAME, 0, (DWORD)verbuf.size(), verbuf.data());

		void* pBlock = verbuf.data();

		// Read the list of languages and code pages.
		struct LANGANDCODEPAGE {
			WORD wLanguage;
			WORD wCodePage;
		} *lpTranslate;
		UINT cbTranslate = sizeof(LANGANDCODEPAGE);

		VerQueryValueA(pBlock, "\\VarFileInfo\\Translation", (LPVOID*)&lpTranslate, &cbTranslate);

		std::vector<char> buf(1024);
		sprintf(buf.data(), "%s%04x%04x%s",
			"\\StringFileInfo\\",
			lpTranslate[0].wLanguage,
			lpTranslate[0].wCodePage,
			"\\ProductVersion");

		// Retrieve file description for language and code page "i". 
		VerQueryValueA(pBlock, buf.data(), &pProductVersion, &lProductVersionSize);
	}
	#endif _WIN32

	// Query Runtime Version
	AMFQueryVersion = (AMFQueryVersion_Fn)GetProcAddress(m_AMFModule, AMF_QUERY_VERSION_FUNCTION_NAME);
	if (!AMFQueryVersion) {
		QUICK_FORMAT_MESSAGE(msg, "Incompatible AMF Runtime (could not find '%s'), error code %ld.",
			AMF_QUERY_VERSION_FUNCTION_NAME,
			GetLastError());
		throw std::exception(msg.data());
	} else {
		AMF_RESULT res = AMFQueryVersion(&m_AMFVersion_Runtime);
		if (res != AMF_OK) {
			QUICK_FORMAT_MESSAGE(msg, "Querying Version failed, error code %ld.",
				res);
			throw std::exception(msg.data());
		}
	}

	/// Initialize AMF
	AMFInit = (AMFInit_Fn)GetProcAddress(m_AMFModule, AMF_INIT_FUNCTION_NAME);
	if (!AMFInit) {
		QUICK_FORMAT_MESSAGE(msg, "Incompatible AMF Runtime (could not find '%s'), error code %ld.",
			AMF_QUERY_VERSION_FUNCTION_NAME,
			GetLastError());
		throw std::exception(msg.data());
	} else {
		AMF_RESULT res = AMFInit(m_AMFVersion_Runtime, &m_AMFFactory);
		if (res != AMF_OK) {
			QUICK_FORMAT_MESSAGE(msg, "Initializing AMF Library failed, error code %ld.",
				res);
			throw std::exception(msg.data());
		}
	}
	AMF_LOG_DEBUG("<" __FUNCTION_NAME__ "> AMF Library initialized.");

	/// Retrieve Trace Object.
	res = m_AMFFactory->GetTrace(&m_AMFTrace);
	if (res != AMF_OK) {
		QUICK_FORMAT_MESSAGE(msg, "Retrieving AMF Trace class failed, error code %ld.",
			res);
		throw std::exception(msg.data());
	}

	/// Retrieve Debug Object.
	res = m_AMFFactory->GetDebug(&m_AMFDebug);
	if (res != AMF_OK) {
		QUICK_FORMAT_MESSAGE(msg, "Retrieving AMF Debug class failed, error code %ld.",
			res);
		throw std::exception(msg.data());
	}

	/// Register Trace Writer and disable Debug Tracing.
	m_TraceWriter = new CustomWriter();
	m_AMFTrace->RegisterWriter(L"OBSWriter", m_TraceWriter, true);
	this->EnableDebugTrace(false);

	// Log success
	AMF_LOG_INFO("Version " PLUGIN_VERSION_TEXT " loaded (Compiled: %d.%d.%d.%d, Runtime: %d.%d.%d.%d, Library: %.*s).",
		(uint16_t)((m_AMFVersion_Plugin >> 48ull) & 0xFFFF),
		(uint16_t)((m_AMFVersion_Plugin >> 32ull) & 0xFFFF),
		(uint16_t)((m_AMFVersion_Plugin >> 16ull) & 0xFFFF),
		(uint16_t)((m_AMFVersion_Plugin & 0xFFFF)),
		(uint16_t)((m_AMFVersion_Runtime >> 48ull) & 0xFFFF),
		(uint16_t)((m_AMFVersion_Runtime >> 32ull) & 0xFFFF),
		(uint16_t)((m_AMFVersion_Runtime >> 16ull) & 0xFFFF),
		(uint16_t)((m_AMFVersion_Runtime & 0xFFFF)),
		lProductVersionSize, pProductVersion
	);

	AMF_LOG_DEBUG("<" __FUNCTION_NAME__ "> Initialized.");
}

Plugin::AMD::AMF::~AMF() {
	AMF_LOG_DEBUG("<" __FUNCTION_NAME__ "> Finalizing.");
	if (m_TraceWriter) {
		//m_AMFTrace->UnregisterWriter(L"OBSWriter");
		delete m_TraceWriter;
		m_TraceWriter = nullptr;
	}

	if (m_AMFModule)
		FreeLibrary(m_AMFModule);
	AMF_LOG_DEBUG("<" __FUNCTION_NAME__ "> Finalized.");

	#pragma region Null Class Members
	m_TimerPeriod = 0;
	m_AMFVersion_Plugin = 0;
	m_AMFVersion_Runtime = 0;
	m_AMFModule = 0;

	m_AMFFactory = nullptr;
	m_AMFTrace = nullptr;
	m_AMFDebug = nullptr;
	AMFQueryVersion = nullptr;
	AMFInit = nullptr;
	#pragma endregion Null Class Members
}

amf::AMFFactory* Plugin::AMD::AMF::GetFactory() {
	return m_AMFFactory;
}

amf::AMFTrace* Plugin::AMD::AMF::GetTrace() {
	return m_AMFTrace;
}

amf::AMFDebug* Plugin::AMD::AMF::GetDebug() {
	return m_AMFDebug;
}

void Plugin::AMD::AMF::EnableDebugTrace(bool enable) {
	if (!m_AMFTrace)
		throw std::exception("<" __FUNCTION_NAME__ "> called without a AMFTrace object!");
	if (!m_AMFDebug)
		throw std::exception("<" __FUNCTION_NAME__ "> called without a AMFDebug object!");

	m_AMFTrace->EnableWriter(AMF_TRACE_WRITER_CONSOLE, false);
	m_AMFTrace->SetWriterLevel(AMF_TRACE_WRITER_CONSOLE, AMF_TRACE_ERROR);
	#ifdef _DEBUG
	m_AMFTrace->EnableWriter(AMF_TRACE_WRITER_DEBUG_OUTPUT, true);
	m_AMFTrace->SetWriterLevel(AMF_TRACE_WRITER_DEBUG_OUTPUT, AMF_TRACE_TEST);
	m_AMFTrace->SetPath(L"C:/AMFTrace.log");
	#else
	m_AMFTrace->EnableWriter(AMF_TRACE_WRITER_DEBUG_OUTPUT, false);
	m_AMFTrace->SetWriterLevel(AMF_TRACE_WRITER_DEBUG_OUTPUT, AMF_TRACE_ERROR);
	#endif
	m_AMFTrace->EnableWriter(AMF_TRACE_WRITER_FILE, false);
	m_AMFTrace->SetWriterLevel(AMF_TRACE_WRITER_FILE, AMF_TRACE_ERROR);

	if (enable) {
		m_AMFDebug->AssertsEnable(true);
		m_AMFDebug->EnablePerformanceMonitor(true);
		m_AMFTrace->TraceEnableAsync(true);
		m_AMFTrace->SetGlobalLevel(AMF_TRACE_TEST);
		m_AMFTrace->SetWriterLevel(L"OBSWriter", AMF_TRACE_TEST);
	} else {
		m_AMFDebug->AssertsEnable(false);
		m_AMFDebug->EnablePerformanceMonitor(false);
		m_AMFTrace->TraceEnableAsync(true);
		m_AMFTrace->SetGlobalLevel(AMF_TRACE_WARNING);
		m_AMFTrace->SetWriterLevel(L"OBSWriter", AMF_TRACE_WARNING);
	}
}