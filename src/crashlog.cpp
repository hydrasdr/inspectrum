/*
 *  Copyright (C) 2026, Benjamin Vernoux <bvernoux@hydrasdr.com>
 *
 *  This file is part of inspectrum.
 *
 *  This program is free software: you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation, either version 3 of the License, or
 *  (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#include "crashlog.h"

#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <mutex>
#include <string>
#include <thread>

#ifdef _WIN32
#  define WIN32_LEAN_AND_MEAN
#  include <windows.h>
#  include <dbghelp.h>
#  include <csignal>  /* signal(SIGABRT) for MinGW abort() */
#else
#  include <csignal>
#  include <unistd.h>
#endif

#include <QCoreApplication>
#include <QSysInfo>
#include <liquid/liquid.h>

/* ------------------------------------------------------------------ */
/*  Internal state                                                     */
/* ------------------------------------------------------------------ */

static std::mutex  g_mutex;
static FILE       *g_file = nullptr;
static std::string g_path;
static std::string g_header;   /* cached system info block */

static const char *severityStr(CrashLog::Severity s)
{
	switch (s) {
	case CrashLog::LOG_INFO:    return "INFO";
	case CrashLog::LOG_WARNING: return "WARN";
	case CrashLog::LOG_ERROR:   return "ERROR";
	case CrashLog::LOG_FATAL:   return "FATAL";
	}
	return "???";
}

/*
 * Write a timestamp into a caller-provided buffer (no heap allocation).
 * Returns the number of characters written (excluding NUL).
 */
static int timestampBuf(char *buf, size_t bufSize)
{
	buf[0] = '\0'; /* safe fallback if strftime fails */
	time_t now = time(nullptr);
	if (now == (time_t)-1)
		return 0;
	struct tm tm;
#ifdef _WIN32
	if (localtime_s(&tm, &now) != 0)
		return 0;
#else
	if (!localtime_r(&now, &tm))
		return 0;
#endif
	int n = (int)strftime(buf, bufSize, "%Y-%m-%d %H:%M:%S", &tm);
	if (n == 0)
		buf[0] = '\0'; /* strftime failed: contents indeterminate */
	return n;
}

/* Convenience wrapper that returns a std::string (for non-crash paths). */
static std::string timestamp()
{
	char buf[64];
	timestampBuf(buf, sizeof(buf));
	return std::string(buf);
}

static std::string threadIdStr()
{
	char buf[32];
	snprintf(buf, sizeof(buf), "%u",
		 (unsigned)(std::hash<std::thread::id>{}(
			 std::this_thread::get_id()) & 0xFFFFFFFF));
	return std::string(buf);
}

/* Build the system-info header that is written once per session. */
static std::string buildHeader(const char *appName, const char *appVersion)
{
	std::string h;
	h.reserve(512);

	h += "========================================\n";
	h += std::string("  ") + appName + " v" + appVersion + "\n";

	/* Compiler */
#if defined(__GNUC__) && !defined(__clang__)
	char comp[64];
	snprintf(comp, sizeof(comp), "GCC %d.%d.%d",
		 __GNUC__, __GNUC_MINOR__, __GNUC_PATCHLEVEL__);
	h += std::string("  Compiler  : ") + comp + "\n";
#elif defined(__clang__)
	char comp[64];
	snprintf(comp, sizeof(comp), "Clang %d.%d.%d",
		 __clang_major__, __clang_minor__, __clang_patchlevel__);
	h += std::string("  Compiler  : ") + comp + "\n";
#elif defined(_MSC_VER)
	char comp[64];
	snprintf(comp, sizeof(comp), "MSVC %d", _MSC_VER);
	h += std::string("  Compiler  : ") + comp + "\n";
#endif

	/* Build type */
#ifdef NDEBUG
	h += "  Build     : Release\n";
#else
	h += "  Build     : Debug\n";
#endif

	/* OS via Qt (works on all platforms) */
	h += std::string("  OS        : ")
	     + QSysInfo::prettyProductName().toStdString() + "\n";
	h += std::string("  Kernel    : ")
	     + QSysInfo::kernelType().toStdString() + " "
	     + QSysInfo::kernelVersion().toStdString() + "\n";
	h += std::string("  Arch      : ")
	     + QSysInfo::currentCpuArchitecture().toStdString() + "\n";

	/* Qt version */
	h += std::string("  Qt        : ") + qVersion() + "\n";

#ifdef __LIQUID_CPUID_H__
	/* liquid-dsp SIMD level (only with bvernoux/liquid-dsp fork) */
	h += std::string("  SIMD      : ")
	     + liquid_simd_level_str(liquid_simd_get_level()) + "\n";
#endif

	h += std::string("  Session   : ") + timestamp() + "\n";
	h += "========================================\n";
	return h;
}

/*
 * Low-level write: assumes caller holds g_mutex **or** is in a crash
 * context where locking is unsafe (heap may be corrupted).
 */
static void writeRaw(const char *str, size_t len)
{
	if (!g_file)
		return;
	fwrite(str, 1, len, g_file);
	fflush(g_file);
}

/* ------------------------------------------------------------------ */
/*  Platform crash handlers                                            */
/*                                                                     */
/*  These run after a fatal signal / exception.  The heap may be       */
/*  corrupted, so every buffer is on the stack. NO std::string,      */
/*  NO new/malloc, NO STL containers.                                  */
/* ------------------------------------------------------------------ */

#ifdef _WIN32

static const char *exceptionCodeStr(DWORD code)
{
	switch (code) {
	case EXCEPTION_ACCESS_VIOLATION:      return "ACCESS_VIOLATION";
	case EXCEPTION_ARRAY_BOUNDS_EXCEEDED: return "ARRAY_BOUNDS_EXCEEDED";
	case EXCEPTION_STACK_OVERFLOW:        return "STACK_OVERFLOW";
	case EXCEPTION_ILLEGAL_INSTRUCTION:   return "ILLEGAL_INSTRUCTION";
	case EXCEPTION_FLT_DIVIDE_BY_ZERO:    return "FLT_DIVIDE_BY_ZERO";
	case EXCEPTION_INT_DIVIDE_BY_ZERO:    return "INT_DIVIDE_BY_ZERO";
	case EXCEPTION_IN_PAGE_ERROR:         return "IN_PAGE_ERROR";
	case EXCEPTION_DATATYPE_MISALIGNMENT: return "DATATYPE_MISALIGNMENT";
	default:                              return "UNKNOWN";
	}
}

static LONG WINAPI unhandledExceptionFilter(EXCEPTION_POINTERS *ep)
{
	/*
	 * Crash context: all buffers on the stack, no heap allocations.
	 * g_mutex is NOT taken: the thread that holds it may be the one
	 * that crashed, so locking would deadlock.
	 */
	char ts[64];
	timestampBuf(ts, sizeof(ts));

	char buf[512];
	int n = snprintf(buf, sizeof(buf),
		"[%s] FATAL  UNHANDLED EXCEPTION: "
		"code=0x%08lX (%s) addr=%p\n",
		ts,
		ep->ExceptionRecord->ExceptionCode,
		exceptionCodeStr(ep->ExceptionRecord->ExceptionCode),
		ep->ExceptionRecord->ExceptionAddress);
	if (n > 0)
		writeRaw(buf, (size_t)n);

	/* Dump first few stack frames if dbghelp is available */
	HANDLE process = GetCurrentProcess();
	if (SymInitialize(process, NULL, TRUE)) {
		CONTEXT *ctx = ep->ContextRecord;
		STACKFRAME64 frame;
		memset(&frame, 0, sizeof(frame));
#if defined(_M_X64) || defined(__x86_64__)
		DWORD machineType = IMAGE_FILE_MACHINE_AMD64;
		frame.AddrPC.Offset    = ctx->Rip;
		frame.AddrFrame.Offset = ctx->Rbp;
		frame.AddrStack.Offset = ctx->Rsp;
#elif defined(_M_IX86) || defined(__i386__)
		DWORD machineType = IMAGE_FILE_MACHINE_I386;
		frame.AddrPC.Offset    = ctx->Eip;
		frame.AddrFrame.Offset = ctx->Ebp;
		frame.AddrStack.Offset = ctx->Esp;
#else
		DWORD machineType = 0;
#endif
		frame.AddrPC.Mode    = AddrModeFlat;
		frame.AddrFrame.Mode = AddrModeFlat;
		frame.AddrStack.Mode = AddrModeFlat;

		writeRaw("  Stack trace:\n", 15);
		for (int i = 0; i < 32; i++) {
			if (!StackWalk64(machineType, process,
					 GetCurrentThread(),
					 &frame, ctx, NULL,
					 SymFunctionTableAccess64,
					 SymGetModuleBase64, NULL))
				break;
			if (frame.AddrPC.Offset == 0)
				break;

			char symBuf[sizeof(SYMBOL_INFO) + 256];
			SYMBOL_INFO *sym = (SYMBOL_INFO *)symBuf;
			sym->SizeOfStruct = sizeof(SYMBOL_INFO);
			sym->MaxNameLen   = 255;

			DWORD64 displacement = 0;
			if (SymFromAddr(process, frame.AddrPC.Offset,
					&displacement, sym)) {
				n = snprintf(buf, sizeof(buf),
					"    #%02d  0x%016llX  %s + 0x%llX\n",
					i,
					(unsigned long long)frame.AddrPC.Offset,
					sym->Name,
					(unsigned long long)displacement);
			} else {
				n = snprintf(buf, sizeof(buf),
					"    #%02d  0x%016llX  (unknown)\n",
					i,
					(unsigned long long)frame.AddrPC.Offset);
			}
			if (n > 0)
				writeRaw(buf, (size_t)n);
		}
		SymCleanup(process);
	}

	return EXCEPTION_CONTINUE_SEARCH;
}

/*
 * Vectored Exception Handler: runs BEFORE frame-based SEH and
 * before the MinGW runtime's signal translator.  This is the only
 * way to catch EXCEPTION_STACK_OVERFLOW on MinGW, because the
 * runtime converts it to SIGSEGV before SetUnhandledExceptionFilter
 * gets called.
 *
 * We only log and continue the search (EXCEPTION_CONTINUE_SEARCH)
 * for fatal exceptions; all others are passed through untouched
 * so normal C++ try/catch and SEH __try/__except still work.
 */
static LONG WINAPI vectoredExceptionHandler(EXCEPTION_POINTERS *ep)
{
	DWORD code = ep->ExceptionRecord->ExceptionCode;

	/* Only intercept fatal hardware exceptions */
	switch (code) {
	case EXCEPTION_ACCESS_VIOLATION:
	case EXCEPTION_STACK_OVERFLOW:
	case EXCEPTION_ILLEGAL_INSTRUCTION:
	case EXCEPTION_INT_DIVIDE_BY_ZERO:
	case EXCEPTION_FLT_DIVIDE_BY_ZERO:
	case EXCEPTION_IN_PAGE_ERROR:
	case EXCEPTION_ARRAY_BOUNDS_EXCEEDED:
	case EXCEPTION_DATATYPE_MISALIGNMENT:
		break;
	default:
		/* Not a crash, let normal handlers process it
		 * (e.g., C++ exceptions, debugger breakpoints) */
		return EXCEPTION_CONTINUE_SEARCH;
	}

	/* Log the crash (same as unhandledExceptionFilter) */
	char ts[64];
	timestampBuf(ts, sizeof(ts));

	char buf[512];
	int n = snprintf(buf, sizeof(buf),
		"[%s] FATAL  UNHANDLED EXCEPTION: "
		"code=0x%08lX (%s) addr=%p\n",
		ts, code, exceptionCodeStr(code),
		ep->ExceptionRecord->ExceptionAddress);
	if (n > 0)
		writeRaw(buf, (size_t)n);

	/* For stack overflow, skip the heavy stack walk, we have
	 * almost no stack left.  For others, try the full trace. */
	if (code != EXCEPTION_STACK_OVERFLOW) {
		HANDLE process = GetCurrentProcess();
		if (SymInitialize(process, NULL, TRUE)) {
			CONTEXT *ctx = ep->ContextRecord;
			STACKFRAME64 frame;
			memset(&frame, 0, sizeof(frame));
#if defined(_M_X64) || defined(__x86_64__)
			DWORD machineType = IMAGE_FILE_MACHINE_AMD64;
			frame.AddrPC.Offset    = ctx->Rip;
			frame.AddrFrame.Offset = ctx->Rbp;
			frame.AddrStack.Offset = ctx->Rsp;
#elif defined(_M_IX86) || defined(__i386__)
			DWORD machineType = IMAGE_FILE_MACHINE_I386;
			frame.AddrPC.Offset    = ctx->Eip;
			frame.AddrFrame.Offset = ctx->Ebp;
			frame.AddrStack.Offset = ctx->Esp;
#else
			DWORD machineType = 0;
#endif
			frame.AddrPC.Mode    = AddrModeFlat;
			frame.AddrFrame.Mode = AddrModeFlat;
			frame.AddrStack.Mode = AddrModeFlat;

			writeRaw("  Stack trace:\n", 15);
			for (int i = 0; i < 32; i++) {
				if (!StackWalk64(machineType, process,
						 GetCurrentThread(),
						 &frame, ctx, NULL,
						 SymFunctionTableAccess64,
						 SymGetModuleBase64, NULL))
					break;
				if (frame.AddrPC.Offset == 0)
					break;

				char symBuf[sizeof(SYMBOL_INFO) + 256];
				SYMBOL_INFO *sym = (SYMBOL_INFO *)symBuf;
				sym->SizeOfStruct = sizeof(SYMBOL_INFO);
				sym->MaxNameLen   = 255;

				DWORD64 displacement = 0;
				if (SymFromAddr(process, frame.AddrPC.Offset,
						&displacement, sym)) {
					n = snprintf(buf, sizeof(buf),
						"    #%02d  0x%016llX  %s + 0x%llX\n",
						i,
						(unsigned long long)frame.AddrPC.Offset,
						sym->Name,
						(unsigned long long)displacement);
				} else {
					n = snprintf(buf, sizeof(buf),
						"    #%02d  0x%016llX  (unknown)\n",
						i,
						(unsigned long long)frame.AddrPC.Offset);
				}
				if (n > 0)
					writeRaw(buf, (size_t)n);
			}
			SymCleanup(process);
		}
	}

	return EXCEPTION_CONTINUE_SEARCH;
}

/*
 * C-level SIGABRT handler for MinGW.
 * MinGW's abort() raises SIGABRT via the C runtime, bypassing SEH.
 */
static void sigabrtHandler(int)
{
	char ts[64];
	timestampBuf(ts, sizeof(ts));
	char buf[256];
	int n = snprintf(buf, sizeof(buf),
		"[%s] FATAL  abort() called (SIGABRT)\n", ts);
	if (n > 0)
		writeRaw(buf, (size_t)n);

	/* Re-raise with default handler */
	signal(SIGABRT, SIG_DFL);
	raise(SIGABRT);
}

static void terminateHandler()
{
	/*
	 * Do NOT call CrashLog::log() here -it takes g_mutex, and if
	 * the crashing thread already holds it we'd deadlock.  Write
	 * directly with stack buffers instead.
	 */
	char ts[64];
	timestampBuf(ts, sizeof(ts));

	char buf[256];
	int n = snprintf(buf, sizeof(buf),
		"[%s] FATAL  std::terminate() called "
		"(uncaught exception or abort)\n", ts);
	if (n > 0)
		writeRaw(buf, (size_t)n);

	abort();
}

#else /* POSIX */

static const char *signalName(int sig)
{
	switch (sig) {
	case SIGSEGV: return "SIGSEGV";
	case SIGABRT: return "SIGABRT";
	case SIGFPE:  return "SIGFPE";
#ifdef SIGBUS
	case SIGBUS:  return "SIGBUS";
#endif
	case SIGILL:  return "SIGILL";
	default:      return "UNKNOWN";
	}
}

static void crashSignalHandler(int sig)
{
	/*
	 * Async-signal-safe: stack buffers only, no heap, no mutex.
	 * fwrite/fflush are technically not AS-safe, but in practice
	 * they work for a single small write to an already-open FILE*.
	 */
	char ts[64];
	timestampBuf(ts, sizeof(ts));

	char buf[256];
	int n = snprintf(buf, sizeof(buf),
		"[%s] FATAL  Received signal %d (%s), crashing\n",
		ts, sig, signalName(sig));
	if (n > 0)
		writeRaw(buf, (size_t)n);

	/* Re-raise with default handler to get core dump */
	signal(sig, SIG_DFL);
	raise(sig);
}

#endif /* _WIN32 */

/* ------------------------------------------------------------------ */
/*  Public API                                                         */
/* ------------------------------------------------------------------ */

void CrashLog::init(const char *appName, const char *appVersion)
{
	std::lock_guard<std::mutex> lock(g_mutex);

	/*
	 * Build log path next to the executable.
	 * Use QCoreApplication (portable across Windows, Linux, macOS).
	 */
	std::string exeDir;
	QString qDir = QCoreApplication::applicationDirPath();
	if (!qDir.isEmpty()) {
		exeDir = qDir.toStdString();
		/* ensure trailing separator */
		if (!exeDir.empty() && exeDir.back() != '/'
#ifdef _WIN32
		    && exeDir.back() != '\\'
#endif
		)
			exeDir += '/';
	}
	if (exeDir.empty())
		exeDir = "./";

	g_path = exeDir + "inspectrum_crash.log";

	/* Open in append mode */
	g_file = fopen(g_path.c_str(), "a");
	if (!g_file) {
		fprintf(stderr, "CrashLog: cannot open %s\n", g_path.c_str());
		return;
	}

	/* Write session header */
	g_header = buildHeader(appName, appVersion);
	writeRaw(g_header.c_str(), g_header.size());
	writeRaw("\n", 1);
}

void CrashLog::log(Severity severity, const char *fmt, ...)
{
	std::lock_guard<std::mutex> lock(g_mutex);
	if (!g_file)
		return;

	/* Timestamp + severity + thread */
	char prefix[128];
	int pn = snprintf(prefix, sizeof(prefix), "[%s] %-5s [thread %s] ",
			  timestamp().c_str(),
			  severityStr(severity),
			  threadIdStr().c_str());
	if (pn > 0)
		writeRaw(prefix, (size_t)pn);

	/* User message */
	char msg[1024];
	va_list ap;
	va_start(ap, fmt);
	int mn = vsnprintf(msg, sizeof(msg), fmt, ap);
	va_end(ap);
	if (mn > 0)
		writeRaw(msg, (size_t)mn);
	writeRaw("\n", 1);
}

void CrashLog::installCrashHandlers()
{
#ifdef _WIN32
	/* Reserve extra stack for exception handling so our handler
	 * can run even during stack overflow. */
	ULONG stackGuarantee = 32768;
	SetThreadStackGuarantee(&stackGuarantee);

	/* Vectored handler: first in chain, catches stack overflow
	 * before MinGW's runtime translates it to SIGSEGV. */
	AddVectoredExceptionHandler(1, vectoredExceptionHandler);

	/* Unhandled exception filter: backup for anything the VEH misses */
	SetUnhandledExceptionFilter(unhandledExceptionFilter);

	/* C-level SIGABRT: catches MinGW abort() which bypasses SEH */
	signal(SIGABRT, sigabrtHandler);

	std::set_terminate(terminateHandler);
#else
	/* Alternate signal stack so the handler can run even when the
	 * main stack overflows (SIGSEGV from stack exhaustion).
	 * Fixed size because SIGSTKSZ is not constant on glibc 2.34+. */
	static char altstack_buf[65536];
	stack_t ss;
	ss.ss_sp = altstack_buf;
	ss.ss_size = sizeof(altstack_buf);
	ss.ss_flags = 0;
	sigaltstack(&ss, nullptr);

	struct sigaction sa;
	memset(&sa, 0, sizeof(sa));
	sa.sa_handler = crashSignalHandler;
	sigemptyset(&sa.sa_mask);
	/* SA_RESETHAND: one-shot, re-raise gets default handler.
	 * SA_ONSTACK: use alternate stack (needed for stack overflow). */
	sa.sa_flags = SA_RESETHAND | SA_ONSTACK;

	sigaction(SIGSEGV, &sa, nullptr);
	sigaction(SIGABRT, &sa, nullptr);
	sigaction(SIGFPE,  &sa, nullptr);
#ifdef SIGBUS
	sigaction(SIGBUS,  &sa, nullptr);
#endif
	sigaction(SIGILL,  &sa, nullptr);

	std::set_terminate([]() {
		/* No CrashLog::log() -would deadlock if mutex is held. */
		char ts[64];
		timestampBuf(ts, sizeof(ts));
		char buf[256];
		int n = snprintf(buf, sizeof(buf),
			"[%s] FATAL  std::terminate() called "
			"(uncaught exception or abort)\n", ts);
		if (n > 0)
			writeRaw(buf, (size_t)n);
		abort();
	});
#endif

	CrashLog::log(LOG_INFO, "Crash handlers installed");
}

const std::string &CrashLog::logFilePath()
{
	return g_path;
}
