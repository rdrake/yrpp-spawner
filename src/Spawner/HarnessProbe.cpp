/**
*  yrpp-spawner
*
*  Copyright(C) 2023-present CnCNet
*
*  This program is free software: you can redistribute it and/or modify
*  it under the terms of the GNU General Public License as published by
*  the Free Software Foundation, either version 3 of the License, or
*  (at your option) any later version.
*
*  This program is distributed in the hope that it will be useful,
*  but WITHOUT ANY WARRANTY; without even the implied warranty of
*  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.See the
*  GNU General Public License for more details.
*
*  You should have received a copy of the GNU General Public License
*  along with this program.If not, see <http://www.gnu.org/licenses/>.
*/

#include "HarnessProbe.h"

#include <Helpers/Macro.h>
#include <Utilities/Debug.h>

#include <ScenarioClass.h>
#include <TechnoClass.h>
#include <Unsorted.h>

#include <Windows.h>
#include <intrin.h>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <cstdint>

bool HarnessProbe::Enable = false;
char HarnessProbe::Dir[HarnessProbe::MaxDirLen] = "HARNESS";

namespace
{
	// ---------------------------------------------------------------------
	// Raw engine addresses that YRpp does not name.
	//
	// The four modal-dialog gate bytes MainLoop tests at 0x55DE4F..0x55DE71.
	// When any is set the frame counter is NOT incremented (the jump to
	// 0x55DEC8 skips it) - but our hook at 0x55DDA0 has already run. They are
	// therefore the empirical signature of "this dispatch fired without
	// advancing the logic frame".
	// ---------------------------------------------------------------------
	inline unsigned char DialogFlagA() { return *reinterpret_cast<const unsigned char*>(0xA83D48); }
	inline unsigned char DialogFlagB() { return *reinterpret_cast<const unsigned char*>(0xA83D49); }
	inline unsigned char DialogFlagC() { return *reinterpret_cast<const unsigned char*>(0xA8ECD0); }
	inline unsigned char DialogFlagD() { return *reinterpret_cast<const unsigned char*>(0x8B41C0); }

	// One buffered observation. Written every invocation, flushed in batches so
	// we never do file IO on most render calls.
	struct ObsRecord
	{
		int Session;
		int Frame;
		int PauseCount;      // Scenario->unknown_62C, the pause nesting counter
		unsigned int Tick;   // GetTickCount(), to expose wall-clock gaps
		unsigned int Esp;    // stack depth - THE re-entrancy signal, see below
		unsigned char IsPaused;
		unsigned char Flags; // bit0..bit3 = dialog gates A..D
	};

	ObsRecord obsBuf[HarnessProbe::MaxObsRecords];
	int obsCount = 0;

	// Per-session state. Everything here resets when the session nonce changes.
	int sessionNonce = 0;
	bool sessionOpen = false;
	int nextCommandId = 1;
	int lastFrame = -1;
	unsigned int lastObsFlushTick = 0;
	unsigned int lastStatusTick = 0;
	int lastObsFlushFrame = 0;
	int lastStatusFrame = 0;
	int hookInvocations = 0;
	int framesAdvanced = 0;
	int noAdvanceInvocations = 0;
	int waitingSinceFrame = 0;
	int lastConsumedId = 0;                 // see DuplicateWatch below
	FILETIME lastConsumedWrite = { 0, 0 };
	bool ended = false;
	bool dirsCreated = false;

	// Error/visibility counters. These are surfaced in status.txt rather than
	// only in the engine debug log, so the evidence bundle the human hands back
	// can never look clean while silently missing records.
	int obsDropped = 0;
	int flushFailures = 0;
	int ackWriteFailures = 0;
	int statusWriteFailures = 0;
	int duplicatesSuppressed = 0;

	// Exactly-once bookkeeping: bit i set once command id (i+1) reached a
	// terminal ack. Fixed storage, never grows.
	unsigned char seenMask[(HarnessProbe::MaxCommands + 7) / 8];

	inline bool WasSeen(int id)
	{
		if (id < 1 || id > HarnessProbe::MaxCommands)
			return true; // out of range ids are treated as already handled
		const int i = id - 1;
		return (seenMask[i >> 3] & (1u << (i & 7))) != 0;
	}

	inline void MarkSeen(int id)
	{
		if (id < 1 || id > HarnessProbe::MaxCommands)
			return;
		const int i = id - 1;
		seenMask[i >> 3] = static_cast<unsigned char>(seenMask[i >> 3] | (1u << (i & 7)));
	}

	inline void ClearSeen(int id)
	{
		if (id < 1 || id > HarnessProbe::MaxCommands)
			return;
		const int i = id - 1;
		seenMask[i >> 3] = static_cast<unsigned char>(seenMask[i >> 3] & ~(1u << (i & 7)));
	}

	void EnsureDirs()
	{
		if (dirsCreated)
			return;
		dirsCreated = true;

		char path[MAX_PATH];
		CreateDirectoryA(HarnessProbe::Dir, nullptr);
		std::snprintf(path, sizeof(path), "%s\\inbox", HarnessProbe::Dir);
		CreateDirectoryA(path, nullptr);
	}

	void FlushObs()
	{
		if (obsCount <= 0)
			return;

		char path[MAX_PATH];
		std::snprintf(path, sizeof(path), "%s\\frames.txt", HarnessProbe::Dir);

		FILE* pFile = std::fopen(path, "at");
		if (!pFile)
		{
			++flushFailures;
			obsDropped += obsCount;
			Debug::Log("[HarnessProbe] Could not append to %s; dropped %d observation(s)\n",
				path, obsCount);
			obsCount = 0;
			return;
		}

		for (int i = 0; i < obsCount; ++i)
		{
			const ObsRecord& r = obsBuf[i];
			std::fprintf(pFile,
				"S=%d F=%d tick=%u pause=%d paused=%d flags=%02X esp=%08X\n",
				r.Session, r.Frame, r.Tick, r.PauseCount,
				static_cast<int>(r.IsPaused),
				static_cast<unsigned int>(r.Flags), r.Esp);
		}

		std::fclose(pFile);
		obsCount = 0;
	}

	void WriteStatus(int frame)
	{
		// Rewritten (not appended) so the controller can read a small file and
		// detect a stalled or exited engine by a frozen tick.
		char tmp[MAX_PATH];
		char path[MAX_PATH];
		std::snprintf(tmp, sizeof(tmp), "%s\\status.tmp", HarnessProbe::Dir);
		std::snprintf(path, sizeof(path), "%s\\status.txt", HarnessProbe::Dir);

		FILE* pFile = std::fopen(tmp, "wt");
		if (!pFile)
		{
			++statusWriteFailures;
			return;
		}

		std::fprintf(pFile,
			"session=%d\nframe=%d\ntick=%u\ninvocations=%d\nframes_advanced=%d\n"
			"no_advance=%d\nnext_command=%d\nwaiting_since_frame=%d\nended=%d\n"
			"obs_dropped=%d\nflush_failures=%d\nack_write_failures=%d\n"
			"status_write_failures=%d\nduplicates_suppressed=%d\n",
			sessionNonce, frame, static_cast<unsigned int>(GetTickCount()),
			hookInvocations, framesAdvanced, noAdvanceInvocations,
			nextCommandId, waitingSinceFrame, ended ? 1 : 0,
			obsDropped, flushFailures, ackWriteFailures,
			statusWriteFailures, duplicatesSuppressed);
		std::fclose(pFile);

		// Atomic publication, same discipline we require of the controller. A
		// reader holding status.txt open without FILE_SHARE_DELETE makes this
		// fail; retry once, then count it so a frozen heartbeat is explainable.
		if (!MoveFileExA(tmp, path, MOVEFILE_REPLACE_EXISTING))
		{
			if (!MoveFileExA(tmp, path, MOVEFILE_REPLACE_EXISTING))
				++statusWriteFailures;
		}
	}

	// Returns false when the ack could not be written. The caller MUST then
	// leave the command unconsumed and retry, otherwise the controller waits
	// forever for a terminal ack that will never appear.
	bool WriteAck(int id, const char* status, const char* reason,
		int requestedFrame, int actualFrame)
	{
		char path[MAX_PATH];
		std::snprintf(path, sizeof(path), "%s\\acks.txt", HarnessProbe::Dir);

		FILE* pFile = std::fopen(path, "at");
		if (!pFile)
		{
			++ackWriteFailures;
			Debug::Log("[HarnessProbe] Could not append ack for id=%d to %s\n", id, path);
			return false;
		}

		// reason must never contain a space - the controller parses this line
		// as whitespace-separated KEY=value tokens.
		std::fprintf(pFile,
			"id=%d status=%s reason=%s requested_frame=%d actual_frame=%d session=%d\n",
			id, status, reason, requestedFrame, actualFrame, sessionNonce);
		std::fclose(pFile);
		return true;
	}

	// Pull one KEY=value line's value out of a NUL-terminated buffer. Matches
	// only at a line start, so `id` never matches `idx=`. Tolerates CRLF, a
	// missing trailing newline, and whitespace around the value.
	bool ReadKey(const char* body, const char* key, char* out, int outSize)
	{
		const int keyLen = static_cast<int>(std::strlen(key));
		const char* p = body;

		while (*p)
		{
			if (std::strncmp(p, key, keyLen) == 0 && p[keyLen] == '=')
			{
				const char* v = p + keyLen + 1;
				while (*v == ' ' || *v == '\t')
					++v;

				int n = 0;
				while (v[n] && v[n] != '\r' && v[n] != '\n' && n < outSize - 1)
				{
					out[n] = v[n];
					++n;
				}
				while (n > 0 && (out[n - 1] == ' ' || out[n - 1] == '\t'))
					--n;
				out[n] = '\0';
				return true;
			}

			while (*p && *p != '\n')
				++p;
			if (*p == '\n')
				++p;
		}

		return false;
	}

	// Watch the most recently consumed command for REPUBLICATION.
	//
	// The scanner is strictly in-order and never re-opens a consumed file, so
	// exactly-once actually rests on nextCommandId monotonicity and the seenMask
	// path is otherwise unreachable - which would make any "duplicate
	// suppressed" claim vacuous. A consumed file also stays on disk, so mere
	// existence proves nothing. What does prove it is the LAST-WRITE TIME: the
	// controller republishes by writing a .tmp and renaming over the original,
	// which changes the timestamp. A changed timestamp on an already-consumed
	// id is a genuine duplicate that we are declining to re-execute.
	bool GetWriteTime(const char* path, FILETIME& out)
	{
		WIN32_FILE_ATTRIBUTE_DATA data;
		if (!GetFileAttributesExA(path, GetFileExInfoStandard, &data))
			return false;
		out = data.ftLastWriteTime;
		return true;
	}

	void DuplicateWatch()
	{
		if (lastConsumedId == 0)
			return;

		char path[MAX_PATH];
		std::snprintf(path, sizeof(path), "%s\\inbox\\%06d.cmd",
			HarnessProbe::Dir, lastConsumedId);

		FILETIME now;
		if (!GetWriteTime(path, now))
			return;

		if (now.dwLowDateTime != lastConsumedWrite.dwLowDateTime ||
			now.dwHighDateTime != lastConsumedWrite.dwHighDateTime)
		{
			lastConsumedWrite = now;
			++duplicatesSuppressed;
			Debug::Log("[HarnessProbe] id=%d republished and suppressed (exactly-once)\n",
				lastConsumedId);
		}
	}

	// Try to consume command `id`. Returns true when a file was found and
	// reached a terminal ack (so the scanner may advance), false when the file
	// is not there yet, is not yet due, or its ack could not be written.
	bool TryConsumeCommand(int id, int frame)
	{
		char path[MAX_PATH];
		std::snprintf(path, sizeof(path), "%s\\inbox\\%06d.cmd", HarnessProbe::Dir, id);

		FILE* pFile = std::fopen(path, "rb");
		if (!pFile)
			return false;

		// The file exists. Because the controller publishes by rename, an
		// openable file is complete - we never see a partial write.
		// Request one byte more than the cap so an exactly-cap-sized file is
		// accepted and only a genuinely larger one is rejected.
		char body[HarnessProbe::MaxCmdFileBytes + 2];
		const size_t n = std::fread(body, 1, HarnessProbe::MaxCmdFileBytes + 1, pFile);
		std::fclose(pFile);
		body[n] = '\0';

		if (WasSeen(id))
		{
			++duplicatesSuppressed;
			return true;
		}
		MarkSeen(id);

		// Every path below either writes exactly one terminal ack, or clears
		// the seen bit and returns false so the command is retried.
		bool acked = true;

		if (n > static_cast<size_t>(HarnessProbe::MaxCmdFileBytes))
		{
			acked = WriteAck(id, "rejected", "oversize", 0, frame);
		}
		else
		{
			char buf[64];

			// --- Stale-session rejection. The restart guard: a command staged
			// for a previous run carries the previous nonce and must never
			// execute against the new game.
			if (!ReadKey(body, "session", buf, sizeof(buf)))
			{
				acked = WriteAck(id, "rejected", "missing-session", 0, frame);
			}
			else if (std::atoi(buf) != sessionNonce)
			{
				acked = WriteAck(id, "rejected", "stale-session", 0, frame);
			}
			else if (!ReadKey(body, "id", buf, sizeof(buf)) || std::atoi(buf) != id)
			{
				// Id must agree with the filename or ordering is meaningless.
				acked = WriteAck(id, "rejected", "id-mismatch", 0, frame);
			}
			else
			{
				int requestedFrame = 0;
				char frameBuf[64];
				if (ReadKey(body, "frame", frameBuf, sizeof(frameBuf)))
					requestedFrame = std::atoi(frameBuf);

				if (!ReadKey(body, "verb", buf, sizeof(buf)))
				{
					acked = WriteAck(id, "rejected", "missing-verb", requestedFrame, frame);
				}
				else if (requestedFrame > 0 && frame > requestedFrame)
				{
					// Late detection. In single-player the engine has no "too
					// late" abort (Execute_DoList only aborts late events when
					// GameMode is not Campaign/Skirmish), so the harness must
					// enforce this itself.
					acked = WriteAck(id, "rejected", "late", requestedFrame, frame);
				}
				else if (requestedFrame > 0 && frame < requestedFrame)
				{
					// Not yet due: defer, and let the scanner retry.
					ClearSeen(id);
					if (waitingSinceFrame == 0)
						waitingSinceFrame = frame;
					return false;
				}
				// --- Execute. Every verb is read-only by construction; this
				// probe never mutates game state or queues events.
				else if (std::strcmp(buf, "noop") == 0)
				{
					acked = WriteAck(id, "executed", "noop", requestedFrame, frame);
				}
				else if (std::strcmp(buf, "count-technos") == 0)
				{
					char reason[64];
					std::snprintf(reason, sizeof(reason), "technos=%d", TechnoClass::Array.Count);
					acked = WriteAck(id, "executed", reason, requestedFrame, frame);
				}
				else if (std::strcmp(buf, "fail-test") == 0)
				{
					// Exercises the fourth ack state so the controller's
					// handling of `failed` is proven in the same play session.
					acked = WriteAck(id, "failed", "deliberate-failure", requestedFrame, frame);
				}
				else if (std::strcmp(buf, "end") == 0)
				{
					acked = WriteAck(id, "executed", "end", requestedFrame, frame);
					if (acked)
					{
						ended = true;
						FlushObs();
						WriteStatus(frame);
					}
				}
				else
				{
					// A terminal rejection, never log-and-skip: an unknown verb
					// must not leave the controller waiting forever.
					acked = WriteAck(id, "rejected", "unknown-verb", requestedFrame, frame);
				}
			}
		}

		if (!acked)
		{
			// Could not record a terminal outcome - do NOT consume the command.
			ClearSeen(id);
			return false;
		}

		waitingSinceFrame = 0;
		lastConsumedId = id;
		if (!GetWriteTime(path, lastConsumedWrite))
			lastConsumedWrite.dwLowDateTime = lastConsumedWrite.dwHighDateTime = 0;
		return true;
	}

	void ResetSession(int nonce)
	{
		// Flush the outgoing session's tail BEFORE dropping it on the floor -
		// the end of a session is exactly where the pause/dialog evidence sits.
		FlushObs();

		sessionNonce = nonce;
		sessionOpen = true;
		nextCommandId = 1;
		hookInvocations = 0;
		framesAdvanced = 0;
		noAdvanceInvocations = 0;
		waitingSinceFrame = 0;
		lastConsumedId = 0;
		lastConsumedWrite.dwLowDateTime = 0;
		lastConsumedWrite.dwHighDateTime = 0;
		obsCount = 0;
		lastObsFlushFrame = 0;
		lastStatusFrame = 0;
		lastObsFlushTick = static_cast<unsigned int>(GetTickCount());
		lastStatusTick = lastObsFlushTick;
		ended = false;
		obsDropped = 0;
		flushFailures = 0;
		ackWriteFailures = 0;
		statusWriteFailures = 0;
		duplicatesSuppressed = 0;
		std::memset(seenMask, 0, sizeof(seenMask));
		Debug::Log("[HarnessProbe] New session, nonce=%d\n", nonce);
	}
}

void HarnessProbe::PerFrame()
{
	if (!Enable)
		return;

	ScenarioClass* pScenario = ScenarioClass::Instance;
	if (!pScenario)
		return;

	const int frame = Unsorted::CurrentFrame;
	if (frame < 0)
		return;

	// --- Session identity. Scenario->UniqueID is a per-game random salt, so a
	// change means a different game - unambiguously, with no false negative
	// when a new game starts at a higher frame than the old one ended.
	//
	// The frame-runs-backwards test is kept as belt-and-braces: if the nonce
	// ever failed to change between games, the stale-session spike would
	// otherwise report a silent false PASS, which is the single most
	// misleading outcome available here.
	const int nonce = pScenario->UniqueID;
	const bool wentBackwards = (lastFrame >= 0 && frame < lastFrame);
	if (!sessionOpen || nonce != sessionNonce || wentBackwards)
	{
		dirsCreated = false;
		EnsureDirs();
		ResetSession(nonce);
		lastFrame = -1;
	}

	++hookInvocations;

	const bool advanced = (frame != lastFrame);
	if (advanced)
		++framesAdvanced;
	else
		++noAdvanceInvocations;
	lastFrame = frame;

	const unsigned int nowTick = static_cast<unsigned int>(GetTickCount());

	// --- Observation record.
	//
	// `esp` is the re-entrancy signal. MainLoop is re-entered from modal UI
	// pumps, but that re-entrancy is NOT observable as nesting of this
	// function: a pump entered earlier in the outer MainLoop has not yet
	// reached our hook, and one entered later has already left it, so our
	// handler is never on the stack twice. What DOES differ is the stack
	// depth - a nested MainLoop activation runs at a markedly lower ESP. So
	// re-entrancy shows up as records at the same frame in a distinctly
	// different ESP band.
	if (obsCount < HarnessProbe::MaxObsRecords)
	{
		ObsRecord& r = obsBuf[obsCount++];
		r.Session = sessionNonce;
		r.Frame = frame;
		r.PauseCount = static_cast<int>(pScenario->unknown_62C);
		r.Tick = nowTick;
		r.Esp = static_cast<unsigned int>(reinterpret_cast<uintptr_t>(_AddressOfReturnAddress()));
		r.IsPaused = static_cast<unsigned char>(pScenario->IsGamePaused ? 1 : 0);
		r.Flags = static_cast<unsigned char>(
			(DialogFlagA() ? 0x01 : 0) |
			(DialogFlagB() ? 0x02 : 0) |
			(DialogFlagC() ? 0x04 : 0) |
			(DialogFlagD() ? 0x08 : 0));
	}
	else
	{
		++obsDropped;
	}

	// --- Flush and heartbeat are keyed on WALL CLOCK as well as frames.
	// Frame-only gating would go silent during a modal dialog (the frame does
	// not advance there), which is precisely one of the windows we are
	// measuring - and a frozen status.txt reads as a dead engine to the
	// controller, aborting an expensive play session.
	if (obsCount >= HarnessProbe::MaxObsRecords ||
		frame - lastObsFlushFrame >= HarnessProbe::ObsFlushInterval ||
		nowTick - lastObsFlushTick >= HarnessProbe::ObsFlushMs)
	{
		FlushObs();
		lastObsFlushFrame = frame;
		lastObsFlushTick = nowTick;
	}

	if (frame - lastStatusFrame >= HarnessProbe::StatusInterval ||
		nowTick - lastStatusTick >= HarnessProbe::StatusMs)
	{
		WriteStatus(frame);
		lastStatusFrame = frame;
		lastStatusTick = nowTick;
	}

	if (ended)
		return;

	DuplicateWatch();

	// --- Bounded inbox service. At most MaxScanPerFrame probes per invocation
	// so we never do uncontrolled work on a render call.
	for (int scanned = 0; scanned < HarnessProbe::MaxScanPerFrame; ++scanned)
	{
		if (nextCommandId > HarnessProbe::MaxCommands)
		{
			// Visible overflow, not a silent stall.
			Debug::Log("[HarnessProbe] Command id cap (%d) reached; refusing further commands\n",
				HarnessProbe::MaxCommands);
			ended = true;
			WriteStatus(frame);
			break;
		}

		if (!TryConsumeCommand(nextCommandId, frame))
			break;

		++nextCommandId;

		if (ended)
			break;
	}
}

// Fourth Syringe tenant at the shared MainLoop dispatch point (SyncDump,
// ProtocolZero and CellDump already hook it; Syringe chains handlers). 5 stolen
// bytes = the full `mov ecx, 0xa8bc60` at 0x55DDA0; 6 would split the following
// call. This point sits after LogicClass::Update (0x55DC9E) and before the
// CurrentFrame increment (0x55DE81), i.e. post(F) with CurrentFrame == F.
DEFINE_HOOK(0x55DDA0, MainLoop_AfterRender__HarnessProbe, 0x5)
{
	HarnessProbe::PerFrame();
	return 0;
}
