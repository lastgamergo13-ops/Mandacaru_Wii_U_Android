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
#include "Cafe/OS/RPL/rpl_symbol_storage.h"
#include "config/ActiveSettings.h"
#include "Cafe/CafeSystem.h"
#include <fstream>
#include <cstring>
#include <cctype>
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

// ---- Mandacaru: EXPORTACAO DE SIMBOLOS E CODIGO DO JOGO (para criar patches de graphic pack) ----
// O modulo do jogo (duniademo) traz os NOMES das funcoes e variaveis. Cerca de 25 s depois da primeira chamada de sistema:
//  1) grava todos os simbolos em  <pasta de dados>/simbolos/<titulo>_simbolos.txt  (endereco, biblioteca, nome) - so se o arquivo nao existe;
//  2) escreve no log, por palavra-chave (distance, shadow, lod, fov, fps, ...), os nomes encontrados com o endereco; para VARIAVEIS mostra o valor atual;
//  3) le  <pasta de dados>/simbolos/dump_request.txt  (opcional): uma linha por pedido, "0x05B6DFAC" (endereco) ou "nome:Trecho" (parte do
//     nome); o codigo PowerPC dessas funcoes (palavras de 32 bits em hexadecimal) sai no log e em <titulo>_codigo.txt.
//     Com o codigo eu monto o patch (ex.: trocar a distancia de visao) sem precisar do jogo desmontado.
static std::string AndroidSymLower(std::string v)
{
	for (auto& ch : v)
		ch = (char)std::tolower((unsigned char)ch);
	return v;
}

static void AndroidSymDumpCode(std::ostream* file, uint32 addr, uint32 endAddr, const std::string& name, uint32 maxWords, bool toLog)
{
	uint32 words = (endAddr > addr) ? (endAddr - addr) / 4 : maxWords;
	if (words > maxWords)
		words = maxWords;
	if (file)
		*file << fmt::format("# {:08x} {} ({} palavras)\n", addr, name, words);
	if (toLog)
		cemuLog_log(LogType::Force, "[Codigo] {:08x} {} ({} palavras)", addr, name, words);
	for (uint32 i = 0; i < words; i += 8)
	{
		std::string line;
		for (uint32 j = i; j < i + 8 && j < words; j++)
		{
			const uint32 a = addr + j * 4;
			line += memory_isAddressRangeAccessible(a, 4) ? fmt::format("{:08X} ", memory_readU32(a)) : std::string("???????? ");
		}
		if (file)
			*file << fmt::format("{:08X}: {}\n", addr + i * 4, line);
		if (toLog)
			cemuLog_log(LogType::Force, "[Codigo]   {:08X}: {}", addr + i * 4, line);
	}
}

static void AndroidSymbolExportThreadMain()
{
	std::this_thread::sleep_for(std::chrono::seconds(25));
	struct Sym { uint32 addr; std::string lib; std::string name; };
	std::vector<Sym> syms;
	{
		auto& symMap = rplSymbolStorage_lockSymbolMap();
		syms.reserve(symMap.size());
		for (auto& kv : symMap)
		{
			RPLStoredSymbol* sym = kv.second;
			if (!sym || !sym->symbolName)
				continue;
			syms.push_back({(uint32)sym->address, sym->libName ? (const char*)sym->libName : "?", (const char*)sym->symbolName});
		}
		rplSymbolStorage_unlockSymbolMap();
	}
	std::sort(syms.begin(), syms.end(), [](const Sym& a, const Sym& b) { return a.addr < b.addr; });
	const uint64 titleId = (uint64)CafeSystem::GetForegroundTitleId();
	std::error_code ec;
	const fs::path dir = ActiveSettings::GetUserDataPath("simbolos");
	fs::create_directories(dir, ec);
	const fs::path symFile = dir / fmt::format("{:016x}_simbolos.txt", titleId);
	const fs::path codeFile = dir / fmt::format("{:016x}_codigo.txt", titleId);
	size_t gameSyms = 0;
	for (const auto& s : syms)
		if (s.lib == "duniademo")
			gameSyms++;
	if (!fs::exists(symFile))
	{
		std::ofstream f(symFile);
		for (const auto& s : syms)
			f << fmt::format("{:08x} {} {}\n", s.addr, s.lib, s.name);
	}
	cemuLog_log(LogType::Force, "[Simbolos] {} simbolos ({} do modulo do jogo) exportados em {}", syms.size(), gameSyms, symFile.string());

	// ---- palavras-chave ----
	static const char* kKeywords[] = {"distance", "viewdist", "drawdist", "farclip", "farplane", "clipplane", "lod", "fov", "shadow", "cascade", "reflect",
		"quality", "detail", "fps", "framerate", "framelimit", "vsync", "swapinterval", "timestep", "deltatime", "ssao", "bloom", "blur", "dof",
		"postfx", "water", "occlusion", "cull", "particle", "density", "traffic", "crowd", "pedestrian", "renderscale", "resolution", "graphicsetting",
		"gamesetting", "videosetting", "option"};
	for (const char* kw : kKeywords)
	{
		std::vector<const Sym*> hits;
		for (const auto& s : syms)
			if (s.lib == "duniademo" && AndroidSymLower(s.name).find(kw) != std::string::npos)
				hits.push_back(&s);
		if (hits.empty())
			continue;
		std::string names;
		const size_t kMax = 10;
		for (size_t i = 0; i < hits.size() && i < kMax; i++)
		{
			std::string n = hits[i]->name.size() > 96 ? hits[i]->name.substr(0, 96) + "..." : hits[i]->name;
			names += fmt::format(" | {:08x} {}", hits[i]->addr, n);
			if (hits[i]->addr >= 0x10000000 && memory_isAddressRangeAccessible(hits[i]->addr, 8))
			{
				const uint32 w0 = memory_readU32(hits[i]->addr);
				float f;
				memcpy(&f, &w0, 4);
				names += fmt::format(" [valor={:08x} float={}]", w0, f);
			}
		}
		cemuLog_log(LogType::Force, "[Simbolos] '{}': {} simbolo(s){}{}", kw, hits.size(), hits.size() > kMax ? " (mostrando 10)" : "", names);
	}

	// ---- pedidos de codigo (dump_request.txt) ----
	const fs::path reqFile = dir / "dump_request.txt";
	std::ifstream req(reqFile);
	if (!req.is_open())
	{
		cemuLog_log(LogType::Force, "[Simbolos] para ver o CODIGO de funcoes, crie {} com linhas como  0x05B6DFAC  ou  nome:Distance  e abra o jogo de novo", reqFile.string());
		return;
	}
	std::ofstream codeOut(codeFile);
	std::string line;
	uint32 dumped = 0;
	while (std::getline(req, line) && dumped < 40)
	{
		while (!line.empty() && (line.back() == '\r' || line.back() == ' '))
			line.pop_back();
		if (line.empty() || line[0] == '#')
			continue;
		std::vector<const Sym*> targets;
		if (line.rfind("nome:", 0) == 0)
		{
			const std::string needle = AndroidSymLower(line.substr(5));
			for (const auto& s : syms)
				if (s.addr >= 0x02000000 && s.addr < 0x10000000 && AndroidSymLower(s.name).find(needle) != std::string::npos)
					targets.push_back(&s);
		}
		else
		{
			const uint32 a = (uint32)std::strtoul(line.c_str(), nullptr, 16);
			for (const auto& s : syms)
				if (s.addr == a)
				{
					targets.push_back(&s);
					break;
				}
			if (targets.empty())
				cemuLog_log(LogType::Force, "[Simbolos] pedido {}: nenhum simbolo nesse endereco exato (use o endereco de inicio da funcao)", line);
		}
		for (const Sym* t : targets)
		{
			if (dumped >= 40)
				break;
			// fim = proximo simbolo de endereco maior
			uint32 endAddr = t->addr + 4 * 600;
			auto it = std::upper_bound(syms.begin(), syms.end(), t->addr, [](uint32 a, const Sym& s) { return a < s.addr; });
			if (it != syms.end() && it->addr < endAddr)
				endAddr = it->addr;
			AndroidSymDumpCode(&codeOut, t->addr, endAddr, t->name, 600, true);
			dumped++;
		}
	}
	cemuLog_log(LogType::Force, "[Simbolos] {} funcao(oes) do dump_request.txt escritas no log e em {}", dumped, codeFile.string());
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
	{
		std::thread(AndroidHleCensusThreadMain).detach();
		std::thread(AndroidSymbolExportThreadMain).detach();
	}
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