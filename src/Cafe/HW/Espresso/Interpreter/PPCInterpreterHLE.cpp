#include "../PPCState.h"
#include "PPCInterpreterInternal.h"
#include "PPCInterpreterHelper.h"

std::unordered_set<std::string> s_unsupportedHLECalls;

void PPCInterpreter_handleUnsupportedHLECall(PPCInterpreter_t* hCPU)
{
	const char* libFuncName = (char*)memory_getPointerFromVirtualOffset(hCPU->instructionPointer + 8);
	std::string tempString = fmt::format("Unsupported lib call: {}", libFuncName);
	if (s_unsupportedHLECalls.find(tempString) == s_unsupportedHLECalls.end())
	{
		// V11.52: no Android a categoria UnsupportedAPI nao pode ser ligada, entao o log nunca mostrava QUAL funcao de biblioteca o jogo
		// chama e o Cemu nao implementa (ex.: Super Mario Galaxy / frisbiiu fica parado). A linha sai uma unica vez por funcao.
		cemuLog_log(LogType::Force, "[APInaoImplementada] {} (chamada ignorada, retorna 0)", tempString);
		s_unsupportedHLECalls.emplace(tempString);
	}
	hCPU->gpr[3] = 0;
	PPCInterpreter_nextInstruction(hCPU);
}

static constexpr size_t HLE_TABLE_CAPACITY = 0x4000;
HLECALL s_ppcHleTable[HLE_TABLE_CAPACITY]{};
std::string s_ppcHleNameTable[HLE_TABLE_CAPACITY];
sint32 s_ppcHleTableWriteIndex = 0;
std::mutex s_ppcHleTableMutex;

HLEIDX PPCInterpreter_registerHLECall(HLECALL hleCall, std::string hleName)
{
	std::unique_lock _l(s_ppcHleTableMutex);
	if (s_ppcHleTableWriteIndex >= HLE_TABLE_CAPACITY)
	{
		cemuLog_log(LogType::Force, "HLE table is full");
		cemu_assert(false);
	}
	for (sint32 i = 0; i < s_ppcHleTableWriteIndex; i++)
	{
		if (s_ppcHleTable[i] == hleCall)
		{
			return i;
		}
	}
	cemu_assert(s_ppcHleTableWriteIndex < HLE_TABLE_CAPACITY);
	s_ppcHleTable[s_ppcHleTableWriteIndex] = hleCall;
	s_ppcHleNameTable[s_ppcHleTableWriteIndex] = std::move(hleName);
	HLEIDX funcIndex = s_ppcHleTableWriteIndex;
	s_ppcHleTableWriteIndex++;
	return funcIndex;
}

HLECALL PPCInterpreter_getHLECall(HLEIDX funcIndex)
{
	if (funcIndex < 0 || funcIndex >= HLE_TABLE_CAPACITY)
		return nullptr;
	return s_ppcHleTable[funcIndex];
}

const std::string& PPCInterpreter_getHLEName(HLEIDX funcIndex)
{
	static const std::string s_empty;
	if (funcIndex < 0 || funcIndex >= HLE_TABLE_CAPACITY)
		return s_empty;
	return s_ppcHleNameTable[funcIndex];
}

#if defined(__ANDROID__)
#include <atomic>
#include <thread>
#include <chrono>
#include <algorithm>
#include "Common/ExceptionHandler/ExceptionHandler.h"
#include "Cafe/OS/libs/coreinit/coreinit_Time.h"
// ---- Mandacaru: REGISTROS DO SISTEMA EMULADO (HLE) ----
// 1) a PRIMEIRA chamada de cada funcao de biblioteca do sistema (coreinit, gx2, nn_*, snd_*, ...) e registrada uma unica vez, com
//    r3..r6 (e o texto, se o argumento aponta para uma string) e o valor devolvido. Sao as entradas/saidas do "sistema" que o jogo ve.
// 2) a cada 30 s: as 20 funcoes mais chamadas no periodo (acha laços de espera, polling) e o tempo emulado x tempo real.
// 3) no log de crash: as 40 funcoes mais chamadas no total.
static std::atomic<uint64> s_hleCensusCount[HLE_TABLE_CAPACITY];
static std::atomic<bool> s_hleCensusThreadStarted{false};

static std::string AndroidHleCensusArg(uint32 v)
{
	std::string out = fmt::format("{:08x}", v);
	if (v >= 0x01000000u && v < 0x50000000u && memory_isAddressRangeAccessible(v, 8))
	{
		const char* p = (const char*)memory_getPointerFromVirtualOffset(v);
		std::string txt;
		for (int i = 0; i < 40; i++)
		{
			const uint32 a = v + i;
			if (!memory_isAddressRangeAccessible(a, 1))
				break;
			const unsigned char ch = (unsigned char)p[i];
			if (ch == 0)
				break;
			if (ch < 0x20 || ch > 0x7e)
			{
				txt.clear();
				break;
			}
			txt += (char)ch;
		}
		if (txt.size() >= 3)
			out += fmt::format("\"{}\"", txt);
	}
	return out;
}

static void AndroidHleCensusThreadMain()
{
	std::vector<uint64> prev(HLE_TABLE_CAPACITY, 0);
	const double espressoClock = 62156250.0; // ESPRESSO_TIMER_CLOCK
	uint64 lastGuestTicks = coreinit::OSGetSystemTime();
	auto lastHost = std::chrono::steady_clock::now();
	for (uint32 round = 1;; round++)
	{
		std::this_thread::sleep_for(std::chrono::seconds(30));
		std::vector<std::pair<uint64, sint32>> deltas;
		uint64 totalDelta = 0;
		const sint32 used = std::min<sint32>(s_ppcHleTableWriteIndex, (sint32)HLE_TABLE_CAPACITY);
		for (sint32 i = 0; i < used; i++)
		{
			const uint64 now = s_hleCensusCount[i].load(std::memory_order_relaxed);
			const uint64 d = now - prev[i];
			prev[i] = now;
			if (d)
			{
				deltas.emplace_back(d, i);
				totalDelta += d;
			}
		}
		std::sort(deltas.begin(), deltas.end(), [](const auto& a, const auto& b) { return a.first > b.first; });
		std::string top;
		for (size_t k = 0; k < deltas.size() && k < 20; k++)
			top += fmt::format(" | {}={}", s_ppcHleNameTable[deltas[k].second], deltas[k].first);
		const auto nowHost = std::chrono::steady_clock::now();
		const double hostSec = std::chrono::duration<double>(nowHost - lastHost).count();
		const uint64 guestTicks = coreinit::OSGetSystemTime();
		const double guestSec = (double)(guestTicks - lastGuestTicks) / espressoClock;
		lastHost = nowHost;
		lastGuestTicks = guestTicks;
		cemuLog_log(LogType::Force, "[Tempo] resumo #{}: {:.2f} s reais = {:.2f} s emulados (razao {:.3f}; <1 = o jogo ve o tempo passar mais devagar que o real)", round, hostSec, guestSec, hostSec > 0 ? guestSec / hostSec : 0.0);
		cemuLog_log(LogType::Force, "[HLE] resumo #{} (30 s): {} chamadas ({}/s), {} funcoes diferentes{}", round, totalDelta, (uint64)(totalDelta / 30), deltas.size(), top);
	}
}

// Conta a chamada. Devolve true se for a PRIMEIRA vez desta funcao (e guarda r3..r6 antes da chamada).
bool AndroidHleCensus_Count(uint32 hleFuncId, PPCInterpreter_t* hCPU, uint32* args)
{
	if (hleFuncId >= HLE_TABLE_CAPACITY)
		return false;
	if (!s_hleCensusThreadStarted.exchange(true))
		std::thread(AndroidHleCensusThreadMain).detach();
	if (s_hleCensusCount[hleFuncId].fetch_add(1, std::memory_order_relaxed) != 0)
		return false;
	args[0] = hCPU->gpr[3];
	args[1] = hCPU->gpr[4];
	args[2] = hCPU->gpr[5];
	args[3] = hCPU->gpr[6];
	return true;
}

void AndroidHleCensus_LogFirst(uint32 hleFuncId, PPCInterpreter_t* hCPU, const uint32* args)
{
	cemuLog_log(LogType::Force, "[HLE] 1a chamada de {}: r3={} r4={} r5={} r6={} => retorno r3={:08x} | LR={:08x}", PPCInterpreter_getHLEName(hleFuncId), AndroidHleCensusArg(args[0]),
		AndroidHleCensusArg(args[1]), AndroidHleCensusArg(args[2]), AndroidHleCensusArg(args[3]), hCPU->gpr[3], hCPU->spr.LR);
}

void AndroidHleCensus_DumpToCrashLog()
{
	std::vector<std::pair<uint64, sint32>> all;
	uint64 total = 0;
	const sint32 used = std::min<sint32>(s_ppcHleTableWriteIndex, (sint32)HLE_TABLE_CAPACITY);
	for (sint32 i = 0; i < used; i++)
	{
		const uint64 n = s_hleCensusCount[i].load();
		if (n)
		{
			all.emplace_back(n, i);
			total += n;
		}
	}
	std::sort(all.begin(), all.end(), [](const auto& a, const auto& b) { return a.first > b.first; });
	CrashLog_WriteLine(fmt::format("[HLE] --- {} chamadas de funcoes do sistema emulado, {} funcoes diferentes; as 40 mais chamadas ---", total, all.size()));
	std::string line;
	for (size_t k = 0; k < all.size() && k < 40; k++)
	{
		line += fmt::format("{}={}  ", s_ppcHleNameTable[all[k].second], all[k].first);
		if ((k % 4) == 3)
		{
			CrashLog_WriteLine(std::string("[HLE]   ") + line);
			line.clear();
		}
	}
	if (!line.empty())
		CrashLog_WriteLine(std::string("[HLE]   ") + line);
}
#endif

std::mutex s_hleLogMutex;

void PPCInterpreter_virtualHLE(PPCInterpreter_t* hCPU, unsigned int opcode)
{
	uint32 hleFuncId = opcode & 0xFFFF;
	if (hleFuncId == 0xFFD0) [[unlikely]]
	{
		s_hleLogMutex.lock();
		PPCInterpreter_handleUnsupportedHLECall(hCPU);
		s_hleLogMutex.unlock();
	}
	else
	{
		// os lib function
		auto hleCall = PPCInterpreter_getHLECall(hleFuncId);
		cemu_assert(hleCall);
#if defined(__ANDROID__)
		uint32 censusArgs[4];
		const bool censusFirst = AndroidHleCensus_Count(hleFuncId, hCPU, censusArgs);
#endif
		hleCall(hCPU);
#if defined(__ANDROID__)
		if (censusFirst)
			AndroidHleCensus_LogFirst(hleFuncId, hCPU, censusArgs);
#endif
	}
}